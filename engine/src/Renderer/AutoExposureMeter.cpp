#include "AutoExposureMeter.h"

#include "AutoExposureMetering.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <Veng/Assert.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Diagnostics/Profiler.h>
#include <Veng/Math/Ease.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/ComputePipeline.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/Types.h>

namespace Veng::Renderer
{
    namespace
    {
        // The auto-exposure metering compute shader (HDR → log-luminance histogram).
        constexpr AssetId AutoExposureCompId{0x9BF9CB926D0595B1ULL};

        // The auto-exposure metering push block, matching auto_exposure.comp PushConstants field for
        // field (every two-component member on an 8-byte boundary, as the shader lays it out): the
        // dynamic-resolution sub-rect mapping, the valid extent, the g-buffer's own sub-rect mapping,
        // the log-luminance histogram bounds, the base element of the ring region this frame
        // accumulates into, and whether a g-buffer is bound.
        struct AutoExposurePush
        {
            vec2 SourceScaleUV;
            vec2 SourceMaxUV;
            uvec2 ValidExtent;
            vec2 OrmScaleUV;
            f32 MinLogLum;
            f32 MaxLogLum;
            u32 HistogramBase;
            u32 ReadsFlags;
        };

        // The metering knobs a view carries, in the form the histogram arithmetic takes them.
        AutoExposureMeterParams MeterParams(const SceneView& view)
        {
            return {
                .FloorLuminance = view.AutoExposureMeterFloor,
                .MaxLuminance = view.AutoExposureMaxLuminance,
                .MinLuminance = view.AutoExposureMinLuminance,
                .LowPercentile = view.AutoExposureLowPercentile,
                .HighPercentile = view.AutoExposureHighPercentile,
                .HighlightPercentile = view.AutoExposureHighlightPercentile,
                .HighlightLuminance = view.AutoExposureHighlightLuminance,
                .Key = view.AutoExposureKey,
            };
        }

        // The dynamic-resolution sub-rect mapping for one mip level of a high-water-mark-allocated
        // chain: the valid extent at that level, and the (scale, clamp) UVs mapping a [0,1] valid
        // UV into the level's valid region. At full resolution ScaleUV is 1 and MaxUV ~1.
        struct MipSubRect
        {
            uvec2 ValidExtent;
            vec2 ScaleUV;
            vec2 MaxUV;
        };

        MipSubRect ComputeMipSubRect(uvec2 validBase, uvec2 allocBase, u32 level)
        {
            const uvec2 valid{std::max(validBase.x >> level, 1u),
                              std::max(validBase.y >> level, 1u)};
            const uvec2 alloc{std::max(allocBase.x >> level, 1u),
                              std::max(allocBase.y >> level, 1u)};
            return {
                .ValidExtent = valid,
                .ScaleUV = vec2(valid) / vec2(alloc),
                .MaxUV = (vec2(valid) - 0.5f) / vec2(alloc),
            };
        }
    }

    Unique<AutoExposureMeter> AutoExposureMeter::Create(Context& context, AssetManager& assets,
                                                        const Ref<ImageView>& hdrView,
                                                        const Ref<ImageView>& ormView)
    {
        return Unique<AutoExposureMeter>(new AutoExposureMeter(context, assets, hdrView, ormView));
    }

    AutoExposureMeter::AutoExposureMeter(Context& context, AssetManager& assets,
                                         const Ref<ImageView>& hdrView,
                                         const Ref<ImageView>& ormView)
        : m_Context(context)
    {
        m_FramesInFlight = m_Context.GetMaxFramesInFlight();

        const AssetResult<AssetHandle<Veng::Shader>> meterCs =
            assets.LoadSync<Veng::Shader>(AutoExposureCompId);
        VE_ASSERT(meterCs.has_value(), "AutoExposureMeter: shader load failed: {}",
                  meterCs.error().Detail);

        // A host-mapped storage ring: one histogram region per frame-in-flight. The metering pass
        // accumulates the current frame's region; the renderer reads the region a completed frame
        // wrote (framesInFlight ago) and re-zeroes it before this frame's pass, so a host read
        // never races a device write still in flight.
        m_Stride = AutoExposureBinCount * sizeof(u32);
        m_Buffer =
            Buffer::Create(m_Context, {
                                          .Name = "SceneRenderer AutoExposure Histogram",
                                          .Size = static_cast<u64>(m_Stride) * m_FramesInFlight,
                                          .Usage = BufferUsage::Storage,
                                          .HostMapped = true,
                                      });
        std::memset(m_Buffer->GetMappedData(), 0, static_cast<usize>(m_Stride) * m_FramesInFlight);

        m_Sampler = Sampler::Create(m_Context, {
                                                   .Name = "SceneRenderer AutoExposure Sampler",
                                                   .MagFilter = Filter::Linear,
                                                   .MinFilter = Filter::Linear,
                                                   .MipmapMode = MipmapMode::Nearest,
                                                   .AddressModeU = AddressMode::ClampToEdge,
                                                   .AddressModeV = AddressMode::ClampToEdge,
                                                   .AddressModeW = AddressMode::ClampToEdge,
                                                   .AnisotropyEnabled = false,
                                               });

        m_SetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer AutoExposure Set Layout",
                           .Bindings =
                               {
                                   {.Binding = 0,
                                    .Type = DescriptorType::SampledImage,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                                   {.Binding = 1,
                                    .Type = DescriptorType::Sampler,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                                   {.Binding = 2,
                                    .Type = DescriptorType::StorageBuffer,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                                   {.Binding = 3,
                                    .Type = DescriptorType::SampledImage,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                               },
                       });
        m_Layout = PipelineLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer AutoExposure Layout",
                           .DescriptorSetLayouts = {m_SetLayout},
                           .PushConstantRanges = {PushConstantRange::Of<AutoExposurePush>(
                               ShaderStage::Compute)},
                       });
        m_Pipeline = ComputePipeline::Create(
            m_Context, {
                           .Name = "SceneRenderer AutoExposure Pipeline",
                           .PipelineLayout = m_Layout,
                           .ShaderStage = {.Stage = ShaderStage::Compute,
                                           .Module = meterCs.value().Get()->Module},
                       });

        Rebind(hdrView, ormView);
    }

    AutoExposureMeter::~AutoExposureMeter() = default;

    void AutoExposureMeter::Rebind(const Ref<ImageView>& hdrView, const Ref<ImageView>& ormView)
    {
        // The HDR target is recreated on every Resize, but the live set may still be referenced
        // by an in-flight frame's command buffer, and its bindings are not update-after-bind —
        // so bind the new view into a fresh set rather than writing the old one in place. The
        // replaced set retires through the deferred-destruction path (the compiled graph's pass
        // capture keeps it alive through its last recorded use), and the Rebuild that follows a
        // Resize recaptures this member.
        m_Set = DescriptorSet::Create(m_Context, {
                                                     .Name = "SceneRenderer AutoExposure Set",
                                                     .Layout = m_SetLayout,
                                                 });
        m_Set->Write(0, hdrView);
        m_Set->Write(1, m_Sampler);
        m_Set->Write(2, m_Buffer);
        // With no g-buffer the binding still needs an image, and the pass is told not to read it.
        m_ReadsFlags = ormView != nullptr;
        m_Set->Write(3, m_ReadsFlags ? ormView : hdrView);
    }

    f32 AutoExposureMeter::ResolveExposure(const SceneView& view, const bool active)
    {
        VE_PROFILE_SCOPE("Render/AutoExposure");

        // Meter the histogram a completed frame wrote (the current frame-in-flight's ring region,
        // written framesInFlight ago and fenced), ease the internal adapted luminance toward what it
        // asks for (eye adaptation), and resolve the exposure the tonemap uses. SceneView::Exposure
        // biases the automatic result; with auto-exposure off it is the exposure directly.
        f32 exposure = view.Exposure;
        if (active)
        {
            const u32 frameIndex = m_Context.GetCurrentFrameInFlight();
            auto* region = reinterpret_cast<u32*>(static_cast<u8*>(m_Buffer->GetMappedData()) +
                                                  static_cast<usize>(frameIndex) * m_Stride);
            const AutoExposureMeterParams params = MeterParams(view);
            const AutoExposureReading reading = MeterAutoExposureHistogram(
                std::span<const u32>(region, AutoExposureBinCount), params);

            // Zero this slot before this frame's metering pass accumulates into it (host-coherent,
            // recorded before the graph submit).
            std::memset(region, 0, AutoExposureBinCount * sizeof(u32));

            // A frame with nothing metered leaves the adaptation and reset pending untouched, so
            // the exposure holds rather than blowing up on an empty meter.
            if (reading.Valid)
            {
                if (m_Reset)
                {
                    m_AdaptedLuminance = reading.Target;
                    m_Reset = false;
                }
                else
                {
                    m_AdaptedLuminance = ExpApproach(m_AdaptedLuminance, reading.Target, view.Delta,
                                                     std::max(view.AutoExposureSpeed, 0.0f));
                }
            }
            // The upper clamp is lifted to the lower rather than trusted to be above it: both ends
            // are authored, independently, by a cooked level and by the debug panel, and an
            // inverted pair is undefined behaviour in std::clamp rather than a bad picture.
            const f32 clamped =
                std::clamp(m_AdaptedLuminance, view.AutoExposureMinLuminance,
                           std::max(view.AutoExposureMaxLuminance, view.AutoExposureMinLuminance));
            exposure = (view.AutoExposureKey / std::max(clamped, 1e-5f)) * view.Exposure;
        }
        m_ResolvedExposure = exposure;
        return exposure;
    }

    void AutoExposureMeter::Declare(RenderGraph& graph, const ResourceId hdrId,
                                    const ResourceId ormId, const ResourceId histogramId,
                                    const uvec2 allocExtent, const uvec2 ormAllocExtent)
    {
        // One thread per lit-HDR texel scatters into a log-luminance histogram. The
        // StorageBufferWrite on the histogram import schedules the pass (it has no image output)
        // and orders it after the HDR write via the graph-derived barrier; the host reads and
        // re-zeroes the histogram a frame later.
        RenderGraph::PassBuilder builder = graph.AddComputePass("Auto Exposure Metering");
        builder.Sample(hdrId);
        const bool readsFlags = m_ReadsFlags && ormId.IsValid();
        if (readsFlags)
        {
            builder.Sample(ormId);
        }
        builder.StorageBufferWrite(histogramId);

        const Ref<ComputePipeline> pipeline = m_Pipeline;
        const Ref<DescriptorSet> set = m_Set;
        const u32 stride = m_Stride;
        builder.Execute(
            [this, pipeline, set, allocExtent, ormAllocExtent, readsFlags,
             stride](PassContext& inner)
            {
                const auto* view = static_cast<const SceneView*>(inner.UserData());
                VE_ASSERT(view != nullptr, "Auto exposure pass: null SceneView");
                // Map pixel-center UVs into the HDR's valid region (identity at full res). The
                // meter reads the post-resolve scene color, so its region is the post-resolve
                // extent — the sub-rect only where the scene color itself is still the sub-rect.
                const MipSubRect r = ComputeMipSubRect(view->PostResolveExtent, allocExtent, 0);
                // The g-buffer is the rendered sub-rect of its own allocation.
                const MipSubRect g = ComputeMipSubRect(view->RenderExtent, ormAllocExtent, 0);
                const u32 frameIndex = m_Context.GetCurrentFrameInFlight();
                CommandBuffer& cmd = inner.Cmd();
                cmd.BindPipeline(pipeline);
                cmd.BindDescriptorSets(DescriptorSetBindInfo{
                    .Sets = {set},
                    .FirstSet = 3,
                    .PipelineBindPoint = PipelineBindPoint::Compute,
                });
                cmd.PushConstants(AutoExposurePush{
                    .SourceScaleUV = r.ScaleUV,
                    .SourceMaxUV = r.MaxUV,
                    .ValidExtent = r.ValidExtent,
                    .OrmScaleUV = g.ScaleUV,
                    .MinLogLum = AutoExposureMinLog(MeterParams(*view)),
                    .MaxLogLum = AutoExposureMaxLog(MeterParams(*view)),
                    .HistogramBase = frameIndex * (stride / static_cast<u32>(sizeof(u32))),
                    .ReadsFlags = readsFlags ? 1U : 0U,
                });
                cmd.Dispatch((r.ValidExtent.x + 15) / 16, (r.ValidExtent.y + 15) / 16, 1);
            });
    }
}
