#include "AutoExposureMeter.h"

#include <algorithm>
#include <cmath>
#include <cstring>

#include <Veng/Assert.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Shader.h>
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

        // Log-luminance histogram resolution: bin 0 is the excluded black bin, bins 1..255 span
        // [MinLogLum, MaxLogLum]. Matches BinCount in auto_exposure.comp.
        constexpr u32 AutoExposureBinCount = 256;

        // The auto-exposure metering push block, matching auto_exposure.comp PushConstants: the
        // dynamic-resolution sub-rect mapping, the valid extent, the log-luminance histogram
        // bounds, and the base element of the ring region this frame accumulates into.
        struct AutoExposurePush
        {
            vec2 SourceScaleUV;
            vec2 SourceMaxUV;
            uvec2 ValidExtent;
            f32 MinLogLum;
            f32 MaxLogLum;
            u32 HistogramBase;
        };

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
                                                        const Ref<ImageView>& hdrView)
    {
        return Unique<AutoExposureMeter>(new AutoExposureMeter(context, assets, hdrView));
    }

    AutoExposureMeter::AutoExposureMeter(Context& context, AssetManager& assets,
                                         const Ref<ImageView>& hdrView)
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

        RebindHdr(hdrView);
    }

    AutoExposureMeter::~AutoExposureMeter() = default;

    void AutoExposureMeter::RebindHdr(const Ref<ImageView>& hdrView)
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
    }

    f32 AutoExposureMeter::ResolveExposure(const SceneView& view, const bool active)
    {
        // Average the log-luminance histogram a completed frame metered (the current
        // frame-in-flight's ring region, written framesInFlight ago and fenced), ease the internal
        // adapted luminance toward it (eye adaptation), and resolve the exposure the tonemap uses.
        // The average excludes bin 0 (the black bin), so a predominantly-black scene with sparse
        // highlights meters on its lit content instead of collapsing to the floor.
        // SceneView::Exposure biases the automatic result. With auto-exposure off, it is the
        // exposure directly.
        f32 exposure = view.Exposure;
        if (active)
        {
            const u32 frameIndex = m_Context.GetCurrentFrameInFlight();
            auto* region = reinterpret_cast<u32*>(static_cast<u8*>(m_Buffer->GetMappedData()) +
                                                  static_cast<usize>(frameIndex) * m_Stride);

            // Weighted average of bins 1..255 (bin centers in log2 space), skipping the black bin
            // and the lit pixels outside the [low, high] percentile band — trimming the tails
            // makes a bimodal frame (a sun-lit surface against a near-black sky) meter on the
            // band rather than the mean of both. The default 0..1 band meters every lit pixel.
            const f32 minLogLum = std::log2(std::max(view.AutoExposureMinLuminance, 1e-5f));
            const f32 maxLogLum = std::log2(std::max(view.AutoExposureMaxLuminance, 1e-4f));
            const f32 logRange = maxLogLum - minLogLum;
            u64 litTotal = 0;
            for (u32 bin = 1; bin < AutoExposureBinCount; ++bin)
            {
                litTotal += region[bin];
            }
            const f32 lowPercentile = std::clamp(view.AutoExposureLowPercentile, 0.0f, 1.0f);
            const f32 highPercentile =
                std::clamp(view.AutoExposureHighPercentile, lowPercentile, 1.0f);
            const f64 lowRank = static_cast<f64>(litTotal) * lowPercentile;
            const f64 highRank = static_cast<f64>(litTotal) * highPercentile;
            f64 weightedLog = 0.0;
            f64 count = 0.0;
            f64 rank = 0.0;
            for (u32 bin = 1; bin < AutoExposureBinCount; ++bin)
            {
                const u32 binCount = region[bin];
                if (binCount == 0)
                {
                    continue;
                }
                // The portion of this bin inside the percentile band (bins straddling an edge
                // contribute fractionally, so the band boundaries do not snap to bins).
                const f64 binLow = rank;
                rank += binCount;
                const f64 inBand =
                    std::max(0.0, std::min(rank, highRank) - std::max(binLow, lowRank));
                if (inBand <= 0.0)
                {
                    continue;
                }
                const f32 t =
                    (static_cast<f32>(bin) - 0.5f) / static_cast<f32>(AutoExposureBinCount - 1);
                weightedLog += static_cast<f64>(minLogLum + t * logRange) * inBand;
                count += inBand;
            }

            // Zero this slot before this frame's metering pass accumulates into it (host-coherent,
            // recorded before the graph submit).
            std::memset(region, 0, AutoExposureBinCount * sizeof(u32));

            // A frame with no lit content (count == 0) leaves the adaptation and reset pending
            // untouched, so the exposure holds rather than blowing up on an empty meter.
            if (count > 0.0)
            {
                const f32 meteredLuminance = std::exp2(static_cast<f32>(weightedLog / count));
                if (m_Reset)
                {
                    m_AdaptedLuminance = meteredLuminance;
                    m_Reset = false;
                }
                else
                {
                    m_AdaptedLuminance =
                        ExpApproach(m_AdaptedLuminance, meteredLuminance, view.Delta,
                                    std::max(view.AutoExposureSpeed, 0.0f));
                }
            }
            const f32 clamped = std::clamp(m_AdaptedLuminance, view.AutoExposureMinLuminance,
                                           view.AutoExposureMaxLuminance);
            exposure = (view.AutoExposureKey / std::max(clamped, 1e-5f)) * view.Exposure;
        }
        m_ResolvedExposure = exposure;
        return exposure;
    }

    void AutoExposureMeter::Declare(RenderGraph& graph, const ResourceId hdrId,
                                    const ResourceId histogramId, const uvec2 allocExtent)
    {
        // One thread per lit-HDR texel scatters into a log-luminance histogram. The
        // StorageBufferWrite on the histogram import schedules the pass (it has no image output)
        // and orders it after the HDR write via the graph-derived barrier; the host reads and
        // re-zeroes the histogram a frame later.
        RenderGraph::PassBuilder builder = graph.AddComputePass("Auto Exposure Metering");
        builder.Sample(hdrId);
        builder.StorageBufferWrite(histogramId);

        const Ref<ComputePipeline> pipeline = m_Pipeline;
        const Ref<DescriptorSet> set = m_Set;
        const u32 stride = m_Stride;
        builder.Execute(
            [this, pipeline, set, allocExtent, stride](PassContext& inner)
            {
                const auto* view = static_cast<const SceneView*>(inner.UserData());
                VE_ASSERT(view != nullptr, "Auto exposure pass: null SceneView");
                // Map pixel-center UVs into the HDR's valid sub-rect (identity at full res).
                const MipSubRect r = ComputeMipSubRect(view->RenderExtent, allocExtent, 0);
                const u32 frameIndex = m_Context.GetCurrentFrameInFlight();
                CommandBuffer& cmd = inner.Cmd();
                cmd.BindPipeline(pipeline);
                cmd.BindDescriptorSets(DescriptorSetBindInfo{
                    .Sets = {set},
                    .FirstSet = 1,
                    .PipelineBindPoint = PipelineBindPoint::Compute,
                });
                cmd.PushConstants(AutoExposurePush{
                    .SourceScaleUV = r.ScaleUV,
                    .SourceMaxUV = r.MaxUV,
                    .ValidExtent = r.ValidExtent,
                    .MinLogLum = std::log2(std::max(view->AutoExposureMinLuminance, 1e-5f)),
                    .MaxLogLum = std::log2(std::max(view->AutoExposureMaxLuminance, 1e-4f)),
                    .HistogramBase = frameIndex * (stride / static_cast<u32>(sizeof(u32))),
                });
                cmd.Dispatch((r.ValidExtent.x + 15) / 16, (r.ValidExtent.y + 15) / 16, 1);
            });
    }
}
