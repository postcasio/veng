// FlowField transports a caller-painted dye along a caller-owned velocity field. It shares FluidSim's
// semi-Lagrangian advection kernel (fluid_advect.comp) and its per-format store family, adding one
// kernel of its own — the clamped unsharp mask (flow_sharpen.comp) that holds detail against the
// blur a feedback advection accrues.

#include <Veng/Renderer/FlowField.h>

#include <cmath>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/ComputePipeline.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>

#include "FlowFieldConfig.h"

namespace Veng::Renderer
{
    namespace
    {
        // The advection kernel and the store family are shared verbatim with FluidSim; the sharpen
        // kernel is FlowField's own. The store stages are a family because a storage image's format
        // qualifier must match the caller's dye image.
        constexpr AssetId AdvectCompId{0xCC8B143D733ED851ULL};
        constexpr AssetId SharpenCompId{0x8CC60D972048009EULL};
        constexpr AssetId StoreR16CompId{0xBB5660E9FEB406ECULL};
        constexpr AssetId StoreRG16CompId{0x95A5A51A78586C78ULL};
        constexpr AssetId StoreRGBA16CompId{0x91261CFBF53859F5ULL};

        constexpr u32 GroupSize = 8;

        // Mirrors FluidParams in fluid_common.slang field for field: the advect and sharpen kernels
        // both include it. TimeStep carries the step scale, and Scale carries the sharpen strength
        // (or the store's 1.0). Relaxation and Flags are inert for FlowField.
        struct FlowPush
        {
            u32 Width;
            u32 Height;
            u32 WrapX;
            u32 WrapY;
            f32 TimeStep;
            f32 Scale;
            f32 Relaxation;
            u32 Flags;
        };

        constexpr u32 Groups(const u32 extent)
        {
            return (extent + GroupSize - 1) / GroupSize;
        }

        FlowFieldImageShape ShapeOf(const Ref<Image>& image)
        {
            if (!image)
            {
                return {};
            }
            const uvec3 extent = image->GetExtent();
            return {.Present = true, .Extent = {extent.x, extent.y}, .Format = image->GetFormat()};
        }

        DescriptorBinding Sampled(const u32 binding)
        {
            return {.Binding = binding,
                    .Type = DescriptorType::SampledImage,
                    .Count = 1,
                    .Stages = ShaderStage::Compute};
        }

        DescriptorBinding Storage(const u32 binding)
        {
            return {.Binding = binding,
                    .Type = DescriptorType::StorageImage,
                    .Count = 1,
                    .Stages = ShaderStage::Compute};
        }

        DescriptorBinding MetricBuffer(const u32 binding)
        {
            return {.Binding = binding,
                    .Type = DescriptorType::StorageBuffer,
                    .Count = 1,
                    .Stages = ShaderStage::Compute};
        }
    }

    Unique<FlowField> FlowField::Create(Context& context, AssetManager& assets,
                                        const FlowFieldInfo& info)
    {
        return Unique<FlowField>(new FlowField(context, assets, info));
    }

    FlowField::FlowField(Context& context, AssetManager& assets, const FlowFieldInfo& info)
        : m_Context(context), m_Name(info.Name), m_Velocity(info.Velocity),
          m_WrapX(info.Shape.WrapX), m_WrapY(info.Shape.WrapY), m_StepScale(info.Shape.StepScale)
    {
        FlowFieldConfig config{
            .Velocity = ShapeOf(info.Velocity),
            .RowMetricCount = info.Shape.RowMetric.size(),
            .StepScale = info.Shape.StepScale,
        };
        config.Dyes.reserve(info.Dyes.size());
        for (const Ref<Image>& dye : info.Dyes)
        {
            config.Dyes.push_back(ShapeOf(dye));
        }

        // A rejected configuration is API misuse, not a recoverable failure: a mismatched extent or
        // an unwritable format means no kernel can bind the field at all.
        if (const VoidResult valid = ValidateFlowFieldConfig(config); !valid.has_value())
        {
            VE_ASSERT(false, "{}", valid.error());
        }

        m_Extent = config.Velocity.Extent;

        auto LoadShader = [&assets](const AssetId id, const char* what) -> Ref<ShaderModule>
        {
            const AssetResult<AssetHandle<Veng::Shader>> result = assets.LoadSync<Veng::Shader>(id);
            VE_ASSERT(result.has_value(), "FlowField: {} shader load failed: {}", what,
                      result.error().Detail);
            return result->Get()->Module;
        };

        m_VelocityView = ImageView::Create(
            m_Context, {.Name = fmt::format("{} Velocity View", m_Name), .Image = m_Velocity});

        // Every advection and sharpen writes this one rgba32f scratch, whatever the dye's own format
        // is: neither can run in place, and a fixed scratch format keeps each a single kernel rather
        // than one per dye format. A per-format store moves the result back out.
        m_Scratch = Image::Create(m_Context, {
                                                 .Name = fmt::format("{} Scratch", m_Name),
                                                 .Extent = {m_Extent.x, m_Extent.y, 1},
                                                 .Format = Format::RGBA32Sfloat,
                                                 .Usage = ImageUsage::Sampled | ImageUsage::Storage,
                                             });
        m_ScratchView = ImageView::Create(
            m_Context, {.Name = fmt::format("{} Scratch View", m_Name), .Image = m_Scratch});

        // One metric entry per row, all ones when the caller supplied none — so the advect kernel
        // needs no "has a metric" branch and the buffer is always bindable.
        vector<f32> metric = info.Shape.RowMetric;
        if (metric.empty())
        {
            metric.assign(m_Extent.y, 1.0f);
        }
        m_RowMetric = Buffer::Create(m_Context, {
                                                    .Name = fmt::format("{} Row Metric", m_Name),
                                                    .Size = metric.size() * sizeof(f32),
                                                    .Usage = BufferUsage::Storage,
                                                });
        m_RowMetric->UploadSync(
            {reinterpret_cast<const u8*>(metric.data()), metric.size() * sizeof(f32)});

        auto MakeLayout = [this](const char* suffix,
                                 vector<DescriptorBinding> bindings) -> Ref<DescriptorSetLayout>
        {
            return DescriptorSetLayout::Create(
                m_Context, {.Name = fmt::format("{} {} Set Layout", m_Name, suffix),
                            .Bindings = std::move(bindings)});
        };
        auto MakePipelineLayout = [this](const char* suffix,
                                         const Ref<DescriptorSetLayout>& set) -> Ref<PipelineLayout>
        {
            return PipelineLayout::Create(
                m_Context,
                {
                    .Name = fmt::format("{} {} Layout", m_Name, suffix),
                    .DescriptorSetLayouts = {set},
                    .PushConstantRanges = {PushConstantRange::Of<FlowPush>(ShaderStage::Compute)},
                });
        };
        auto MakePipeline = [this](const char* suffix, const Ref<PipelineLayout>& layout,
                                   const Ref<ShaderModule>& module) -> Ref<ComputePipeline>
        {
            return ComputePipeline::Create(
                m_Context, {
                               .Name = fmt::format("{} {} Pipeline", m_Name, suffix),
                               .PipelineLayout = layout,
                               .ShaderStage = {.Stage = ShaderStage::Compute, .Module = module},
                           });
        };

        m_AdvectSetLayout =
            MakeLayout("Advect", {Sampled(0), Sampled(1), Storage(2), MetricBuffer(3)});
        m_AdvectLayout = MakePipelineLayout("Advect", m_AdvectSetLayout);
        m_AdvectPipeline =
            MakePipeline("Advect", m_AdvectLayout, LoadShader(AdvectCompId, "advect"));

        // The sharpen and store kernels share one two-image layout (a sampled source, a storage
        // destination); the sharpen binds dye->scratch, the store scratch->dye.
        m_ImageSetLayout = MakeLayout("Image", {Sampled(0), Storage(1)});
        m_ImageLayout = MakePipelineLayout("Image", m_ImageSetLayout);
        m_SharpenPipeline =
            MakePipeline("Sharpen", m_ImageLayout, LoadShader(SharpenCompId, "sharpen"));

        m_Dyes.reserve(info.Dyes.size());
        for (usize i = 0; i < info.Dyes.size(); ++i)
        {
            const Ref<Image>& source = info.Dyes[i];
            const string label = fmt::format("Dye {}", i);

            Dye dye;
            dye.Field = source;
            dye.View = ImageView::Create(
                m_Context, {.Name = fmt::format("{} {} View", m_Name, label), .Image = source});

            AssetId storeId = StoreR16CompId;
            if (source->GetFormat() == Format::RG16Sfloat)
            {
                storeId = StoreRG16CompId;
            }
            else if (source->GetFormat() == Format::RGBA16Sfloat)
            {
                storeId = StoreRGBA16CompId;
            }
            dye.StorePipeline = MakePipeline(fmt::format("{} Store", label).c_str(), m_ImageLayout,
                                             LoadShader(storeId, "dye store"));

            dye.AdvectSet = DescriptorSet::Create(
                m_Context, {.Name = fmt::format("{} {} Advect Set", m_Name, label),
                            .Layout = m_AdvectSetLayout});
            dye.AdvectSet->Write(0, dye.View);
            dye.AdvectSet->Write(1, m_VelocityView);
            dye.AdvectSet->Write(2, m_ScratchView);
            dye.AdvectSet->Write(3, m_RowMetric);

            dye.SharpenSet = DescriptorSet::Create(
                m_Context, {.Name = fmt::format("{} {} Sharpen Set", m_Name, label),
                            .Layout = m_ImageSetLayout});
            dye.SharpenSet->Write(0, dye.View);
            dye.SharpenSet->Write(1, m_ScratchView);

            dye.StoreSet = DescriptorSet::Create(
                m_Context, {.Name = fmt::format("{} {} Store Set", m_Name, label),
                            .Layout = m_ImageSetLayout});
            dye.StoreSet->Write(0, m_ScratchView);
            dye.StoreSet->Write(1, dye.View);

            m_Dyes.push_back(std::move(dye));
        }
    }

    FlowField::~FlowField() = default;

    void FlowField::SetStepScale(const f32 stepScale)
    {
        VE_ASSERT(std::isfinite(stepScale) && stepScale > 0.0f,
                  "FlowField '{}': the step scale {} is not positive and finite", m_Name,
                  stepScale);
        m_StepScale = stepScale;
    }

    void FlowField::Bind(CommandBuffer& cmd, const Ref<ComputePipeline>& pipeline,
                         const Ref<DescriptorSet>& set, const f32 scale) const
    {
        cmd.BindPipeline(pipeline);
        cmd.BindDescriptorSets(DescriptorSetBindInfo{
            .Sets = {set},
            .FirstSet = 3,
            .PipelineBindPoint = PipelineBindPoint::Compute,
        });
        cmd.PushConstants(FlowPush{
            .Width = m_Extent.x,
            .Height = m_Extent.y,
            .WrapX = static_cast<u32>(m_WrapX),
            .WrapY = static_cast<u32>(m_WrapY),
            .TimeStep = m_StepScale,
            .Scale = scale,
            .Relaxation = 0.0f,
            .Flags = 0,
        });
    }

    void FlowField::DispatchGrid(CommandBuffer& cmd) const
    {
        cmd.Dispatch(Groups(m_Extent.x), Groups(m_Extent.y), 1);
    }

    void FlowField::RecordAdvect(CommandBuffer& cmd, const u32 steps)
    {
        for (u32 step = 0; step < steps; ++step)
        {
            RecordAdvect(cmd);
        }
    }

    void FlowField::RecordAdvect(CommandBuffer& cmd)
    {
        // Prepare the velocity for sampling from its tracked state each advect, not once: a caller
        // may have rewritten the image since the last advect, so this barrier makes that write
        // visible to the sample below (and orders this read ahead of the caller's next write).
        // **SampleCompute, because the advect below is a dispatch** — naming the fragment stage
        // here would leave a caller's next write ordered against a stage that read nothing.
        cmd.PrepareForAccess(m_VelocityView, AccessKind::SampleCompute);
        for (const Dye& dye : m_Dyes)
        {
            RecordDyeAdvect(cmd, dye);
        }
        m_AdvectCount++;
    }

    void FlowField::RecordSharpen(CommandBuffer& cmd, const f32 strength)
    {
        if (strength == 0.0f)
        {
            return;
        }
        for (const Dye& dye : m_Dyes)
        {
            RecordDyeSharpen(cmd, dye, strength);
        }
    }

    void FlowField::RecordDyeAdvect(CommandBuffer& cmd, const Dye& dye)
    {
        cmd.PrepareForAccess(dye.View, AccessKind::SampleCompute);
        cmd.PrepareForAccess(m_ScratchView, AccessKind::StorageWrite);
        Bind(cmd, m_AdvectPipeline, dye.AdvectSet, 1.0f);
        DispatchGrid(cmd);

        cmd.PrepareForAccess(m_ScratchView, AccessKind::SampleCompute);
        cmd.PrepareForAccess(dye.View, AccessKind::StorageWrite);
        Bind(cmd, dye.StorePipeline, dye.StoreSet, 1.0f);
        DispatchGrid(cmd);
        // The release: a dye is the caller's image and this primitive does not know what reads it
        // next — the following advect's dispatch, or a material sampling it in a fragment.
        cmd.PrepareForAccess(dye.View, AccessKind::SampleAny);
    }

    void FlowField::RecordDyeSharpen(CommandBuffer& cmd, const Dye& dye, const f32 strength)
    {
        cmd.PrepareForAccess(dye.View, AccessKind::SampleCompute);
        cmd.PrepareForAccess(m_ScratchView, AccessKind::StorageWrite);
        Bind(cmd, m_SharpenPipeline, dye.SharpenSet, strength);
        DispatchGrid(cmd);

        cmd.PrepareForAccess(m_ScratchView, AccessKind::SampleCompute);
        cmd.PrepareForAccess(dye.View, AccessKind::StorageWrite);
        Bind(cmd, dye.StorePipeline, dye.StoreSet, 1.0f);
        DispatchGrid(cmd);
        cmd.PrepareForAccess(dye.View, AccessKind::SampleAny);
    }

    i32 FoldFlowTexel(const i32 coord, const u32 extent, const FlowWrap wrap)
    {
        const i32 length = static_cast<i32>(extent);
        if (wrap == FlowWrap::Periodic)
        {
            const i32 folded = coord % length;
            return folded < 0 ? folded + length : folded;
        }
        return std::clamp(coord, 0, length - 1);
    }

    vec2 FlowBackTrace(const uvec2 texel, const vec2 velocity, const f32 stepScale,
                       const f32 rowMetric)
    {
        const vec2 centre = vec2(static_cast<f32>(texel.x), static_cast<f32>(texel.y)) + 0.5f;
        return centre - (stepScale * vec2(velocity.x * rowMetric, velocity.y));
    }

    vec4 SampleFlowBilinear(const vec2 position, const uvec2 extent, const FlowWrap wrapX,
                            const FlowWrap wrapY, const function<vec4(ivec2)>& load)
    {
        const vec2 offset = position - 0.5f;
        const vec2 origin = glm::floor(offset);
        const vec2 fraction = offset - origin;
        const ivec2 base(static_cast<i32>(origin.x), static_cast<i32>(origin.y));

        auto Tap = [&](const i32 dx, const i32 dy)
        {
            return load({FoldFlowTexel(base.x + dx, extent.x, wrapX),
                         FoldFlowTexel(base.y + dy, extent.y, wrapY)});
        };

        const vec4 lower = glm::mix(Tap(0, 0), Tap(1, 0), fraction.x);
        const vec4 upper = glm::mix(Tap(0, 1), Tap(1, 1), fraction.x);
        return glm::mix(lower, upper, fraction.y);
    }

    vec4 FlowSharpen(const vec4 centre, const vec4 blur, const vec4 neighbourhoodMin,
                     const vec4 neighbourhoodMax, const f32 strength)
    {
        const vec4 sharpened = centre + strength * (centre - blur);
        return glm::clamp(sharpened, neighbourhoodMin, neighbourhoodMax);
    }
}
