// The stable-fluids scheme is Jos Stam's, reimplemented from his published papers ("Stable
// Fluids", SIGGRAPH 1999, and "Real-Time Fluid Dynamics for Games", GDC 2003): semi-Lagrangian
// advection, then forces, then a Jacobi pressure projection. The mathematics is reimplemented;
// no code was taken from any implementation of it.

#include <Veng/Renderer/FluidSim.h>

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

#include "FluidSimShape.h"

namespace Veng::Renderer
{
    namespace
    {
        // Core fluid compute shaders (engine/assets/core/shaders/fluid/*.comp.slang). The force,
        // gradient and store stages are families rather than single kernels because a storage
        // image's format qualifier must match the image it writes, and those three write the
        // caller's own fields.
        constexpr AssetId AdvectCompId{0xCC8B143D733ED851ULL};
        constexpr AssetId CurlCompId{0x0FF6D4D6031B3A93ULL};
        constexpr AssetId DivergenceCompId{0x1EBD6BA0C390AD64ULL};
        constexpr AssetId JacobiCompId{0xDAB7BDBBC17B8EC0ULL};
        constexpr AssetId ForcesRG16CompId{0x8C2D87D6B86AB109ULL};
        constexpr AssetId ForcesRG32CompId{0x3600119D4E791247ULL};
        constexpr AssetId GradientRG16CompId{0xB286EB1C8F507BC5ULL};
        constexpr AssetId GradientRG32CompId{0x933684ECFE2EE090ULL};
        constexpr AssetId StoreR16CompId{0xBB5660E9FEB406ECULL};
        constexpr AssetId StoreRG16CompId{0x95A5A51A78586C78ULL};
        constexpr AssetId StoreRGBA16CompId{0x91261CFBF53859F5ULL};

        constexpr u32 GroupSize = 8;

        // Mirrors FluidParams in fluid_common.slang field for field.
        struct FluidPush
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

        constexpr u32 FlagFirstIteration = 1u;
        constexpr u32 FlagHasTarget = 2u;
        constexpr u32 FlagHasDamping = 4u;

        constexpr u32 Groups(const u32 extent)
        {
            return (extent + GroupSize - 1) / GroupSize;
        }

        FluidFieldShape ShapeOf(const Ref<Image>& image)
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

    Unique<FluidSim> FluidSim::Create(Context& context, AssetManager& assets,
                                      const FluidSimInfo& info)
    {
        return Unique<FluidSim>(new FluidSim(context, assets, info));
    }

    FluidSim::FluidSim(Context& context, AssetManager& assets, const FluidSimInfo& info)
        : m_Context(context), m_Name(info.Name), m_Velocity(info.Velocity), m_WrapX(info.WrapX),
          m_WrapY(info.WrapY), m_TimeStep(info.TimeStep), m_JacobiIterations(info.JacobiIterations),
          m_Confinement(info.VorticityConfinement), m_RelaxationRate(info.RelaxationRate),
          m_VelocityDissipation(info.VelocityDissipation),
          m_HasTarget(info.RelaxationTarget != nullptr), m_HasDamping(info.DampingMask != nullptr)
    {
        FluidSimShape shape{
            .Velocity = ShapeOf(info.Velocity),
            .RelaxationTarget = ShapeOf(info.RelaxationTarget),
            .DampingMask = ShapeOf(info.DampingMask),
            .RowMetricCount = info.RowMetric.size(),
            .JacobiIterations = info.JacobiIterations,
            .TimeStep = info.TimeStep,
        };
        shape.Dyes.reserve(info.Dyes.size());
        for (const FluidDyeInfo& dye : info.Dyes)
        {
            shape.Dyes.push_back(ShapeOf(dye.Field));
        }

        // A rejected configuration is API misuse, not a recoverable failure: a mismatched extent
        // or an unwritable format means no kernel can bind the field at all.
        if (const VoidResult valid = ValidateFluidSimShape(shape); !valid.has_value())
        {
            VE_ASSERT(false, "{}", valid.error());
        }

        m_Extent = shape.Velocity.Extent;

        auto LoadShader = [&assets](const AssetId id, const char* what) -> Ref<ShaderModule>
        {
            const AssetResult<AssetHandle<Veng::Shader>> result = assets.LoadSync<Veng::Shader>(id);
            VE_ASSERT(result.has_value(), "FluidSim: {} shader load failed: {}", what,
                      result.error().Detail);
            return result->Get()->Module;
        };

        auto MakeImage = [this](const char* suffix, const Format format) -> Ref<Image>
        {
            return Image::Create(m_Context, {
                                                .Name = fmt::format("{} {}", m_Name, suffix),
                                                .Extent = {m_Extent.x, m_Extent.y, 1},
                                                .Format = format,
                                                .Usage = ImageUsage::Sampled | ImageUsage::Storage,
                                            });
        };
        auto MakeView = [this](const char* suffix, const Ref<Image>& image) -> Ref<ImageView>
        {
            return ImageView::Create(
                m_Context, {.Name = fmt::format("{} {} View", m_Name, suffix), .Image = image});
        };

        m_VelocityView = MakeView("Velocity", m_Velocity);

        // Every advection writes this one rgba32f scratch, whatever the advected field's own
        // format is: semi-Lagrangian advection cannot run in place, and a fixed scratch format
        // keeps the advection a single kernel rather than one per field format.
        m_Scratch = MakeImage("Scratch", Format::RGBA32Sfloat);
        m_ScratchView = MakeView("Scratch", m_Scratch);
        m_Curl = MakeImage("Curl", Format::R32Sfloat);
        m_CurlView = MakeView("Curl", m_Curl);
        m_Divergence = MakeImage("Divergence", Format::R32Sfloat);
        m_DivergenceView = MakeView("Divergence", m_Divergence);
        m_Pressure[0] = MakeImage("Pressure A", Format::R32Sfloat);
        m_PressureView[0] = MakeView("Pressure A", m_Pressure[0]);
        m_Pressure[1] = MakeImage("Pressure B", Format::R32Sfloat);
        m_PressureView[1] = MakeView("Pressure B", m_Pressure[1]);

        // The optional fields are always bound, so an absent one gets a 1x1 stand-in the shader's
        // flag gate never reads.
        m_Placeholder =
            Image::Create(m_Context, {
                                         .Name = fmt::format("{} Placeholder", m_Name),
                                         .Extent = {1, 1, 1},
                                         .Format = Format::R16Sfloat,
                                         .Usage = ImageUsage::Sampled | ImageUsage::Storage,
                                     });
        m_PlaceholderView = MakeView("Placeholder", m_Placeholder);
        m_TargetView =
            m_HasTarget ? MakeView("Relaxation Target", info.RelaxationTarget) : m_PlaceholderView;
        m_DampingView =
            m_HasDamping ? MakeView("Damping Mask", info.DampingMask) : m_PlaceholderView;

        // One metric entry per row, all ones when the caller supplied none — so no kernel needs a
        // "has a metric" branch and the buffer is always bindable.
        vector<f32> metric = info.RowMetric;
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
                    .PushConstantRanges = {PushConstantRange::Of<FluidPush>(ShaderStage::Compute)},
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
        auto MakeSet = [this](const char* suffix,
                              const Ref<DescriptorSetLayout>& layout) -> Ref<DescriptorSet>
        {
            return DescriptorSet::Create(
                m_Context, {.Name = fmt::format("{} {} Set", m_Name, suffix), .Layout = layout});
        };

        m_AdvectSetLayout =
            MakeLayout("Advect", {Sampled(0), Sampled(1), Storage(2), MetricBuffer(3)});
        m_AdvectLayout = MakePipelineLayout("Advect", m_AdvectSetLayout);
        m_AdvectPipeline =
            MakePipeline("Advect", m_AdvectLayout, LoadShader(AdvectCompId, "advect"));

        m_FieldSetLayout = MakeLayout("Field", {Sampled(0), Storage(1), MetricBuffer(2)});
        m_FieldLayout = MakePipelineLayout("Field", m_FieldSetLayout);
        m_CurlPipeline = MakePipeline("Curl", m_FieldLayout, LoadShader(CurlCompId, "curl"));
        m_DivergencePipeline =
            MakePipeline("Divergence", m_FieldLayout, LoadShader(DivergenceCompId, "divergence"));

        m_JacobiSetLayout =
            MakeLayout("Jacobi", {Sampled(0), Sampled(1), Storage(2), MetricBuffer(3)});
        m_JacobiLayout = MakePipelineLayout("Jacobi", m_JacobiSetLayout);
        m_JacobiPipeline =
            MakePipeline("Jacobi", m_JacobiLayout, LoadShader(JacobiCompId, "jacobi"));

        const bool wideVelocity = shape.Velocity.Format == Format::RG32Sfloat;

        m_ForcesSetLayout = MakeLayout("Forces", {Sampled(0), Sampled(1), Sampled(2), Sampled(3),
                                                  Storage(4), MetricBuffer(5)});
        m_ForcesLayout = MakePipelineLayout("Forces", m_ForcesSetLayout);
        m_ForcesPipeline =
            MakePipeline("Forces", m_ForcesLayout,
                         LoadShader(wideVelocity ? ForcesRG32CompId : ForcesRG16CompId, "forces"));

        m_GradientSetLayout = MakeLayout("Gradient", {Sampled(0), Storage(1), MetricBuffer(2)});
        m_GradientLayout = MakePipelineLayout("Gradient", m_GradientSetLayout);
        m_GradientPipeline = MakePipeline(
            "Gradient", m_GradientLayout,
            LoadShader(wideVelocity ? GradientRG32CompId : GradientRG16CompId, "gradient"));

        m_StoreSetLayout = MakeLayout("Store", {Sampled(0), Storage(1)});
        m_StoreLayout = MakePipelineLayout("Store", m_StoreSetLayout);

        m_AdvectVelocitySet = MakeSet("Advect Velocity", m_AdvectSetLayout);
        m_AdvectVelocitySet->Write(0, m_VelocityView);
        m_AdvectVelocitySet->Write(1, m_VelocityView);
        m_AdvectVelocitySet->Write(2, m_ScratchView);
        m_AdvectVelocitySet->Write(3, m_RowMetric);

        m_CurlSet = MakeSet("Curl", m_FieldSetLayout);
        m_CurlSet->Write(0, m_ScratchView);
        m_CurlSet->Write(1, m_CurlView);
        m_CurlSet->Write(2, m_RowMetric);

        m_ForcesSet = MakeSet("Forces", m_ForcesSetLayout);
        m_ForcesSet->Write(0, m_CurlView);
        m_ForcesSet->Write(1, m_ScratchView);
        m_ForcesSet->Write(2, m_TargetView);
        m_ForcesSet->Write(3, m_DampingView);
        m_ForcesSet->Write(4, m_VelocityView);
        m_ForcesSet->Write(5, m_RowMetric);

        m_DivergenceSet = MakeSet("Divergence", m_FieldSetLayout);
        m_DivergenceSet->Write(0, m_VelocityView);
        m_DivergenceSet->Write(1, m_DivergenceView);
        m_DivergenceSet->Write(2, m_RowMetric);

        for (u32 source = 0; source < 2; ++source)
        {
            m_JacobiSet[source] = MakeSet(source == 0 ? "Jacobi A" : "Jacobi B", m_JacobiSetLayout);
            m_JacobiSet[source]->Write(0, m_PressureView[source]);
            m_JacobiSet[source]->Write(1, m_DivergenceView);
            m_JacobiSet[source]->Write(2, m_PressureView[1 - source]);
            m_JacobiSet[source]->Write(3, m_RowMetric);

            m_GradientSet[source] =
                MakeSet(source == 0 ? "Gradient A" : "Gradient B", m_GradientSetLayout);
            m_GradientSet[source]->Write(0, m_PressureView[source]);
            m_GradientSet[source]->Write(1, m_VelocityView);
            m_GradientSet[source]->Write(2, m_RowMetric);
        }

        m_Dyes.reserve(info.Dyes.size());
        for (usize i = 0; i < info.Dyes.size(); ++i)
        {
            const FluidDyeInfo& source = info.Dyes[i];
            const string label = fmt::format("Dye {}", i);

            Dye dye;
            dye.Field = source.Field;
            dye.Dissipation = source.Dissipation;
            dye.View =
                ImageView::Create(m_Context, {.Name = fmt::format("{} {} View", m_Name, label),
                                              .Image = source.Field});

            AssetId storeId = StoreR16CompId;
            if (source.Field->GetFormat() == Format::RG16Sfloat)
            {
                storeId = StoreRG16CompId;
            }
            else if (source.Field->GetFormat() == Format::RGBA16Sfloat)
            {
                storeId = StoreRGBA16CompId;
            }
            dye.StorePipeline = ComputePipeline::Create(
                m_Context, {
                               .Name = fmt::format("{} {} Store Pipeline", m_Name, label),
                               .PipelineLayout = m_StoreLayout,
                               .ShaderStage = {.Stage = ShaderStage::Compute,
                                               .Module = LoadShader(storeId, "dye store")},
                           });

            dye.AdvectSet = DescriptorSet::Create(
                m_Context, {.Name = fmt::format("{} {} Advect Set", m_Name, label),
                            .Layout = m_AdvectSetLayout});
            dye.AdvectSet->Write(0, dye.View);
            dye.AdvectSet->Write(1, m_VelocityView);
            dye.AdvectSet->Write(2, m_ScratchView);
            dye.AdvectSet->Write(3, m_RowMetric);

            dye.StoreSet = DescriptorSet::Create(
                m_Context, {.Name = fmt::format("{} {} Store Set", m_Name, label),
                            .Layout = m_StoreSetLayout});
            dye.StoreSet->Write(0, m_ScratchView);
            dye.StoreSet->Write(1, dye.View);

            m_Dyes.push_back(std::move(dye));
        }
    }

    FluidSim::~FluidSim() = default;

    void FluidSim::SetTimeStep(const f32 timeStep)
    {
        VE_ASSERT(std::isfinite(timeStep) && timeStep > 0.0f,
                  "FluidSim '{}': the timestep {} is not positive and finite", m_Name, timeStep);
        m_TimeStep = timeStep;
    }

    void FluidSim::SetJacobiIterations(const u32 iterations)
    {
        VE_ASSERT(iterations > 0,
                  "FluidSim '{}': the projection needs at least one Jacobi iteration", m_Name);
        m_JacobiIterations = iterations;
    }

    void FluidSim::SetDyeDissipation(const u32 index, const f32 dissipation)
    {
        VE_ASSERT(index < m_Dyes.size(), "FluidSim '{}': dye {} does not exist ({} supplied)",
                  m_Name, index, m_Dyes.size());
        m_Dyes[index].Dissipation = dissipation;
    }

    void FluidSim::Bind(CommandBuffer& cmd, const Ref<ComputePipeline>& pipeline,
                        const Ref<DescriptorSet>& set, const f32 scale, const u32 flags) const
    {
        cmd.BindPipeline(pipeline);
        cmd.BindDescriptorSets(DescriptorSetBindInfo{
            .Sets = {set},
            .FirstSet = 3,
            .PipelineBindPoint = PipelineBindPoint::Compute,
        });
        cmd.PushConstants(FluidPush{
            .Width = m_Extent.x,
            .Height = m_Extent.y,
            .WrapX = static_cast<u32>(m_WrapX),
            .WrapY = static_cast<u32>(m_WrapY),
            .TimeStep = m_TimeStep,
            .Scale = scale,
            .Relaxation = m_RelaxationRate,
            .Flags = flags,
        });
    }

    void FluidSim::DispatchGrid(CommandBuffer& cmd) const
    {
        cmd.Dispatch(Groups(m_Extent.x), Groups(m_Extent.y), 1);
    }

    void FluidSim::RecordSteps(CommandBuffer& cmd, const u32 steps)
    {
        for (u32 step = 0; step < steps; ++step)
        {
            RecordStep(cmd);
        }
    }

    void FluidSim::RecordStep(CommandBuffer& cmd)
    {
        RecordVelocity(cmd);
        RecordProjection(cmd);

        cmd.PrepareForAccess(m_VelocityView, AccessKind::Sample);
        for (const Dye& dye : m_Dyes)
        {
            RecordDye(cmd, dye);
        }

        m_StepCount++;
    }

    void FluidSim::RecordVelocity(CommandBuffer& cmd)
    {
        // Advect the velocity through itself into the scratch.
        cmd.PrepareForAccess(m_VelocityView, AccessKind::Sample);
        cmd.PrepareForAccess(m_ScratchView, AccessKind::StorageWrite);
        Bind(cmd, m_AdvectPipeline, m_AdvectVelocitySet, 1.0f, 0);
        DispatchGrid(cmd);

        // Curl of the advected velocity, the field confinement steers by.
        cmd.PrepareForAccess(m_ScratchView, AccessKind::Sample);
        cmd.PrepareForAccess(m_CurlView, AccessKind::StorageWrite);
        Bind(cmd, m_CurlPipeline, m_CurlSet, 0.0f, 0);
        DispatchGrid(cmd);

        // Forces, back into the caller's velocity image. The dissipation folds in here rather
        // than into the advection, because the advection's destination is the shared scratch and
        // the force stage is the pass that lands the result.
        u32 flags = 0;
        if (m_HasTarget)
        {
            flags |= FlagHasTarget;
        }
        if (m_HasDamping)
        {
            flags |= FlagHasDamping;
        }
        cmd.PrepareForAccess(m_CurlView, AccessKind::Sample);
        cmd.PrepareForAccess(m_TargetView, AccessKind::Sample);
        cmd.PrepareForAccess(m_DampingView, AccessKind::Sample);
        cmd.PrepareForAccess(m_VelocityView, AccessKind::StorageWrite);
        Bind(cmd, m_ForcesPipeline, m_ForcesSet, m_Confinement, flags);
        DispatchGrid(cmd);
    }

    void FluidSim::RecordProjection(CommandBuffer& cmd)
    {
        cmd.PrepareForAccess(m_VelocityView, AccessKind::Sample);
        cmd.PrepareForAccess(m_DivergenceView, AccessKind::StorageWrite);
        Bind(cmd, m_DivergencePipeline, m_DivergenceSet, 0.0f, 0);
        DispatchGrid(cmd);

        // The solve restarts from zero pressure each step: iteration 0 reads no neighbours, so
        // neither pressure image needs clearing and the step is a pure function of its inputs.
        // The source is still transitioned on that iteration — a bound sampled descriptor must
        // name a subresource in the layout it was written with whether or not the shader reads
        // it, and on the first step of a solver's life that image is still Undefined.
        cmd.PrepareForAccess(m_DivergenceView, AccessKind::Sample);
        u32 source = 0;
        for (u32 iteration = 0; iteration < m_JacobiIterations; ++iteration)
        {
            const u32 destination = 1 - source;
            cmd.PrepareForAccess(m_PressureView[source], AccessKind::Sample);
            cmd.PrepareForAccess(m_PressureView[destination], AccessKind::StorageWrite);
            Bind(cmd, m_JacobiPipeline, m_JacobiSet[source], 0.0f,
                 iteration == 0 ? FlagFirstIteration : 0);
            DispatchGrid(cmd);
            source = destination;
        }

        // `source` now names the image the last iteration wrote.
        cmd.PrepareForAccess(m_PressureView[source], AccessKind::Sample);
        cmd.PrepareForAccess(m_VelocityView, AccessKind::StorageWrite);
        Bind(cmd, m_GradientPipeline, m_GradientSet[source], 0.0f, 0);
        DispatchGrid(cmd);
    }

    void FluidSim::RecordDye(CommandBuffer& cmd, const Dye& dye)
    {
        cmd.PrepareForAccess(dye.View, AccessKind::Sample);
        cmd.PrepareForAccess(m_ScratchView, AccessKind::StorageWrite);
        Bind(cmd, m_AdvectPipeline, dye.AdvectSet, 1.0f, 0);
        DispatchGrid(cmd);

        const f32 survival = 1.0f - (dye.Dissipation * m_TimeStep);
        cmd.PrepareForAccess(m_ScratchView, AccessKind::Sample);
        cmd.PrepareForAccess(dye.View, AccessKind::StorageWrite);
        Bind(cmd, dye.StorePipeline, dye.StoreSet, survival, 0);
        DispatchGrid(cmd);
        cmd.PrepareForAccess(dye.View, AccessKind::Sample);
    }

    i32 FoldFluidTexel(const i32 coord, const u32 extent, const FluidWrap wrap)
    {
        const i32 length = static_cast<i32>(extent);
        if (wrap == FluidWrap::Periodic)
        {
            const i32 folded = coord % length;
            return folded < 0 ? folded + length : folded;
        }
        return std::clamp(coord, 0, length - 1);
    }

    vec2 FluidBackTrace(const uvec2 texel, const vec2 velocity, const f32 timeStep,
                        const f32 rowMetric)
    {
        const vec2 centre = vec2(static_cast<f32>(texel.x), static_cast<f32>(texel.y)) + 0.5f;
        return centre - (timeStep * vec2(velocity.x * rowMetric, velocity.y));
    }

    vec4 SampleFluidBilinear(const vec2 position, const uvec2 extent, const FluidWrap wrapX,
                             const FluidWrap wrapY, const function<vec4(ivec2)>& load)
    {
        const vec2 offset = position - 0.5f;
        const vec2 origin = glm::floor(offset);
        const vec2 fraction = offset - origin;
        const ivec2 base(static_cast<i32>(origin.x), static_cast<i32>(origin.y));

        auto Tap = [&](const i32 dx, const i32 dy)
        {
            return load({FoldFluidTexel(base.x + dx, extent.x, wrapX),
                         FoldFluidTexel(base.y + dy, extent.y, wrapY)});
        };

        const vec4 lower = glm::mix(Tap(0, 0), Tap(1, 0), fraction.x);
        const vec4 upper = glm::mix(Tap(0, 1), Tap(1, 1), fraction.x);
        return glm::mix(lower, upper, fraction.y);
    }
}
