#include "BloomPyramid.h"

#include <algorithm>
#include <bit>

#include <fmt/format.h>

#include <Veng/Assert.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/ComputePipeline.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/Types.h>

#include "AutoExposureMeter.h"

namespace Veng::Renderer
{
    namespace
    {
        // The core bloom compute shaders (downsample, upsample-accumulate, composite) and the
        // Dual Kawase variants of the down/up sweep.
        constexpr AssetId BloomDownCompId{0x5B8811BEAC5D9C3BULL};
        constexpr AssetId BloomUpCompId{0x4F28282A720BC9F2ULL};
        constexpr AssetId BloomCompositeCompId{0x533236398AB7654FULL};
        constexpr AssetId BloomDownKawaseCompId{0xCB1AA796A1E3BBEFULL};
        constexpr AssetId BloomUpKawaseCompId{0x0C269FA0D5F353D2ULL};

        // Linear float HDR format for the bloom pyramid; matches the lighting target format.
        constexpr Format HdrFormat = Format::RGBA16Sfloat;
        constexpr ImageUsage HdrUsage = ImageUsage::ColorAttachment | ImageUsage::Sampled;

        // The bloom pyramid's image usage: a compute-written, compute-read mip chain.
        constexpr ImageUsage BloomPyramidUsage = ImageUsage::Storage | ImageUsage::Sampled;

        // The coarsest pyramid level holds a ~8 px edge (2^3) rather than a degenerate
        // 1×1 contributing nothing: the chain stops BloomTileShift levels short.
        constexpr u32 BloomTileShift = 3;

        // The bloom downsample push: the destination mip extent, a level-0 bright-pass
        // flag (1.0 enables bright-pass + Karis), the soft-knee threshold, and the level-0
        // bloom-mask slots. Matches bloom_down.comp and bloom_down_kawase.comp.
        struct BloomDownPush
        {
            uvec2 DestExtent;
            vec2 SourceScaleUV; // source validExtent/allocExtent (dynamic-resolution sub-rect)
            vec2 SourceMaxUV;   // source (validExtent-0.5)/allocExtent (bilinear-tap clamp)
            f32 BrightPass;
            f32 Threshold;
            u32 MaskTexture;
            u32 MaskSampler;
            u32 MaskEnabled;
        };

        // The bloom upsample push: the destination (finer) mip extent, the source sub-rect
        // mapping, and the spread scale. Matches bloom_up.comp.
        struct BloomUpPush
        {
            uvec2 DestExtent;
            vec2 SourceScaleUV;
            vec2 SourceMaxUV;
            f32 Radius;
        };

        // The bloom composite push: the result extent, the source sub-rect mapping (shared by
        // the HDR and bloom-mip-0 inputs, both at mip-0 scale), and the bloom mix. Matches
        // bloom_composite.comp.
        struct BloomCompositePush
        {
            uvec2 DestExtent;
            vec2 SourceScaleUV;
            vec2 SourceMaxUV;
            f32 Intensity;
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

    Unique<BloomPyramid> BloomPyramid::Create(Context& context, AssetManager& assets,
                                              const BloomKernel kernel)
    {
        return Unique<BloomPyramid>(new BloomPyramid(context, assets, kernel));
    }

    BloomPyramid::BloomPyramid(Context& context, AssetManager& assets, const BloomKernel kernel)
        : m_Context(context), m_Kernel(kernel)
    {
        auto LoadShader = [&](const AssetId id, const char* what) -> AssetHandle<Veng::Shader>
        {
            const AssetResult<AssetHandle<Veng::Shader>> result = assets.LoadSync<Veng::Shader>(id);
            VE_ASSERT(result.has_value(), "BloomPyramid: {} shader load failed: {}", what,
                      result.error().Detail);
            return *result;
        };

        // The bloom compute pipelines. Set 1 is off bindless (the closed bloom chain needs
        // no global registration, and a dedicated set sidesteps the set-0 storage-image
        // argument-buffer path on MoltenVK). Down/up share one set layout (sampled source +
        // linear sampler + storage dest); composite needs a distinct one (two sampled inputs).
        const AssetHandle<Veng::Shader> bloomDownCs =
            LoadShader(BloomDownCompId, "bloom downsample");
        const AssetHandle<Veng::Shader> bloomUpCs = LoadShader(BloomUpCompId, "bloom upsample");
        const AssetHandle<Veng::Shader> bloomDownKawaseCs =
            LoadShader(BloomDownKawaseCompId, "bloom downsample (Kawase)");
        const AssetHandle<Veng::Shader> bloomUpKawaseCs =
            LoadShader(BloomUpKawaseCompId, "bloom upsample (Kawase)");
        const AssetHandle<Veng::Shader> bloomCompositeCs =
            LoadShader(BloomCompositeCompId, "bloom composite");

        m_DownUpSetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer Bloom DownUp Set Layout",
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
                                    .Type = DescriptorType::StorageImage,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                               },
                       });
        m_CompositeSetLayout = DescriptorSetLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer Bloom Composite Set Layout",
                           .Bindings =
                               {
                                   {.Binding = 0,
                                    .Type = DescriptorType::SampledImage,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                                   {.Binding = 1,
                                    .Type = DescriptorType::SampledImage,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                                   {.Binding = 2,
                                    .Type = DescriptorType::Sampler,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                                   {.Binding = 3,
                                    .Type = DescriptorType::StorageImage,
                                    .Count = 1,
                                    .Stages = ShaderStage::Compute},
                               },
                       });

        m_DownUpLayout = PipelineLayout::Create(
            m_Context,
            {
                .Name = "SceneRenderer Bloom DownUp Layout",
                .DescriptorSetLayouts = {m_DownUpSetLayout},
                .PushConstantRanges = {PushConstantRange::Of<BloomDownPush>(ShaderStage::Compute)},
            });
        m_CompositeLayout = PipelineLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer Bloom Composite Layout",
                           .DescriptorSetLayouts = {m_CompositeSetLayout},
                           .PushConstantRanges = {PushConstantRange::Of<BloomCompositePush>(
                               ShaderStage::Compute)},
                       });

        m_DownPipeline = ComputePipeline::Create(
            m_Context,
            {
                .Name = "SceneRenderer Bloom Down Pipeline",
                .PipelineLayout = m_DownUpLayout,
                .ShaderStage = {.Stage = ShaderStage::Compute, .Module = bloomDownCs.Get()->Module},
            });
        m_UpPipeline = ComputePipeline::Create(
            m_Context,
            {
                .Name = "SceneRenderer Bloom Up Pipeline",
                .PipelineLayout = m_DownUpLayout,
                .ShaderStage = {.Stage = ShaderStage::Compute, .Module = bloomUpCs.Get()->Module},
            });
        m_DownKawasePipeline = ComputePipeline::Create(
            m_Context, {
                           .Name = "SceneRenderer Bloom Down Kawase Pipeline",
                           .PipelineLayout = m_DownUpLayout,
                           .ShaderStage = {.Stage = ShaderStage::Compute,
                                           .Module = bloomDownKawaseCs.Get()->Module},
                       });
        m_UpKawasePipeline = ComputePipeline::Create(
            m_Context, {
                           .Name = "SceneRenderer Bloom Up Kawase Pipeline",
                           .PipelineLayout = m_DownUpLayout,
                           .ShaderStage = {.Stage = ShaderStage::Compute,
                                           .Module = bloomUpKawaseCs.Get()->Module},
                       });
        m_CompositePipeline = ComputePipeline::Create(
            m_Context, {
                           .Name = "SceneRenderer Bloom Composite Pipeline",
                           .PipelineLayout = m_CompositeLayout,
                           .ShaderStage = {.Stage = ShaderStage::Compute,
                                           .Module = bloomCompositeCs.Get()->Module},
                       });
    }

    BloomPyramid::~BloomPyramid()
    {
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_ResultHandle);
        bindless.Release(m_Mip0Handle);
    }

    void BloomPyramid::Resize(const uvec2 extent, const Ref<ImageView>& hdrView)
    {
        // Bloom operates in linear HDR space before tonemap, sampling bilinearly: the wide
        // COD/tent taps land between texels, so the pyramid's HdrFormat must advertise linear
        // filtering — a capability the point-Load hi-Z reduction never exercises.
        VE_ASSERT(m_Context.IsFormatLinearFilterSupported(HdrFormat),
                  "BloomPyramid: bloom needs SampledImageFilterLinear on the HDR format");

        m_Extent = extent;

        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_ResultHandle);
        bindless.Release(m_Mip0Handle);

        // The pyramid stops BloomTileShift levels short of 1×1 so the coarsest level holds a
        // ~8 px edge; the max(1u, …) floor guards a tiny extent (mirroring hi-Z's guard).
        const u32 maxDim = std::max(extent.x, extent.y);
        const u32 mipCount =
            maxDim == 0 ? 1u : std::max(1u, std::bit_width(maxDim) - BloomTileShift);

        m_Image = Image::Create(m_Context, {
                                               .Name = "SceneRenderer Bloom Pyramid",
                                               .Extent = {extent.x, extent.y, 1},
                                               .MipLevels = mipCount,
                                               .Format = HdrFormat,
                                               .Usage = BloomPyramidUsage,
                                           });

        // One single-mip storage view per level (the down/up dispatches write each), plus a
        // whole-chain sampled view for the bilinear reads. Storage and sampled access to one
        // mip need distinct views.
        m_Mips.clear();
        m_Mips.reserve(mipCount);
        for (u32 level = 0; level < mipCount; level++)
        {
            m_Mips.push_back(ImageView::Create(
                m_Context, {
                               .Name = fmt::format("SceneRenderer Bloom Mip {} View", level),
                               .Image = m_Image,
                               .BaseMipLevel = level,
                               .MipLevels = 1,
                           }));
        }
        m_SampleView = ImageView::Create(m_Context, {
                                                        .Name = "SceneRenderer Bloom Sample View",
                                                        .Image = m_Image,
                                                        .MipLevels = mipCount,
                                                    });

        // Clamp-to-edge linear sampler for the bilinear taps; MaxLod covers every level so a
        // per-mip sampled source view's level 0 always resolves.
        m_Sampler = Sampler::Create(m_Context, {
                                                   .Name = "SceneRenderer Bloom Sampler",
                                                   .MagFilter = Filter::Linear,
                                                   .MinFilter = Filter::Linear,
                                                   .MipmapMode = MipmapMode::Nearest,
                                                   .AddressModeU = AddressMode::ClampToEdge,
                                                   .AddressModeV = AddressMode::ClampToEdge,
                                                   .AddressModeW = AddressMode::ClampToEdge,
                                                   .AnisotropyEnabled = false,
                                                   .MaxLod = static_cast<f32>(mipCount),
                                               });

        // The full-resolution composite result the tonemap samples; registered into bindless.
        m_ResultImage = Image::Create(m_Context, {
                                                     .Name = "SceneRenderer Bloom Result",
                                                     .Extent = {extent.x, extent.y, 1},
                                                     .Format = HdrFormat,
                                                     .Usage = HdrUsage | ImageUsage::Storage,
                                                 });
        m_ResultView = ImageView::Create(
            m_Context, {.Name = "SceneRenderer Bloom Result", .Image = m_ResultImage});
        m_ResultHandle = bindless.Register(m_ResultView);

        // The DebugView::Bloom arm samples pyramid mip 0 (post up-sweep) as a bindless source.
        m_Mip0Handle = bindless.Register(m_Mips[0]);

        // Per-level descriptor sets. A down step binds the source (HDR for level 0, mip k-1
        // otherwise) + sampler + the destination mip; an up step binds the coarser mip k+1 +
        // sampler + the finer destination mip k.
        m_DownSets.clear();
        m_DownSets.reserve(mipCount);
        for (u32 level = 0; level < mipCount; level++)
        {
            Ref<DescriptorSet> set = DescriptorSet::Create(
                m_Context, {
                               .Name = fmt::format("SceneRenderer Bloom Down Set {}", level),
                               .Layout = m_DownUpSetLayout,
                           });
            const Ref<ImageView>& source = level == 0 ? hdrView : m_Mips[level - 1];
            set->Write(0, source);
            set->Write(1, m_Sampler);
            set->Write(2, m_Mips[level]);
            m_DownSets.push_back(std::move(set));
        }

        // Up sets indexed by the finer destination level k (0..mipCount-2): read mip k+1,
        // write mip k.
        m_UpSets.clear();
        if (mipCount > 1)
        {
            m_UpSets.reserve(mipCount - 1);
            for (u32 level = 0; level + 1 < mipCount; level++)
            {
                Ref<DescriptorSet> set = DescriptorSet::Create(
                    m_Context, {
                                   .Name = fmt::format("SceneRenderer Bloom Up Set {}", level),
                                   .Layout = m_DownUpSetLayout,
                               });
                set->Write(0, m_Mips[level + 1]);
                set->Write(1, m_Sampler);
                set->Write(2, m_Mips[level]);
                m_UpSets.push_back(std::move(set));
            }
        }

        // Composite: HDR + bloom mip 0 sampled inputs, the linear sampler, and the result dest.
        m_CompositeSet =
            DescriptorSet::Create(m_Context, {
                                                 .Name = "SceneRenderer Bloom Composite Set",
                                                 .Layout = m_CompositeSetLayout,
                                             });
        m_CompositeSet->Write(0, hdrView);
        m_CompositeSet->Write(1, m_Mips[0]);
        m_CompositeSet->Write(2, m_Sampler);
        m_CompositeSet->Write(3, m_ResultView);
    }

    void BloomPyramid::Declare(RenderGraph& graph, const ResourceId hdrId, const MipChainId chainId,
                               const ResourceId resultId, const AutoExposureMeter& autoExposure,
                               const ResourceId maskId, const TextureHandle maskHandle,
                               const SamplerHandle maskSampler)
    {
        const u32 mipCount = static_cast<u32>(m_Mips.size());
        const uvec2 allocExtent = m_Extent;

        // The mask is folded in only when the renderer supplied a live target and both slots
        // resolved; without it level 0 is the luminance bright-pass alone.
        const bool maskActive = maskId.IsValid() && maskHandle.IsValid() && maskSampler.IsValid();

        // Down-sweep: dispatch k samples level k's source (HDR for k=0, mip k-1 otherwise)
        // and writes mip k. Mip 0 fuses the bright-pass + Karis; deeper levels are the plain
        // 13-tap. The per-mip graph surface derives the read-after-write barrier between
        // dispatch k's write and dispatch k+1's read.
        for (u32 level = 0; level < mipCount; level++)
        {
            RenderGraph::PassBuilder builder =
                graph.AddComputePass(fmt::format("Bloom Down Mip {}", level));
            if (level == 0)
            {
                builder.Sample(hdrId);
                if (maskActive)
                {
                    builder.Sample(maskId);
                }
            }
            else
            {
                builder.Sample(chainId.Level(level - 1));
            }
            builder.StorageWrite(chainId.Level(level));

            const Ref<ComputePipeline> pipeline =
                m_Kernel == BloomKernel::Kawase ? m_DownKawasePipeline : m_DownPipeline;
            const Ref<DescriptorSet> set = m_DownSets[level];
            const f32 brightPass = level == 0 ? 1.0f : 0.0f;
            // Only level 0 bright-passes, so only level 0 reads the mask.
            const bool levelMask = maskActive && level == 0;
            Context* context = &m_Context;
            // The source is mip level-1 (the HDR for level 0); its dynamic-resolution sub-rect
            // ratio is at the source's level. Computed at record time from this frame's extent.
            const u32 srcLevel = level == 0 ? 0u : level - 1;
            const AutoExposureMeter* meter = &autoExposure;
            builder.Execute(
                [pipeline, set, level, srcLevel, allocExtent, brightPass, meter, levelMask,
                 maskHandle, maskSampler, context](PassContext& inner)
                {
                    const auto* view = static_cast<const SceneView*>(inner.UserData());
                    VE_ASSERT(view != nullptr, "Bloom down pass: null SceneView");
                    const MipSubRect dst =
                        ComputeMipSubRect(view->RenderExtent, allocExtent, level);
                    const MipSubRect src =
                        ComputeMipSubRect(view->RenderExtent, allocExtent, srcLevel);
                    CommandBuffer& cmd = inner.Cmd();
                    cmd.BindPipeline(pipeline);
                    if (levelMask)
                    {
                        // The mask is the one bloom input off the registry rather than off the
                        // chain's own set, so the registry has to be bound for this dispatch.
                        context->GetBindlessRegistry().Bind(cmd, PipelineBindPoint::Compute);
                    }
                    cmd.BindDescriptorSets(DescriptorSetBindInfo{
                        .Sets = {set},
                        .FirstSet = 3, // sets 0-2 are the typed bindless registries
                        .PipelineBindPoint = PipelineBindPoint::Compute,
                    });
                    cmd.PushConstants(BloomDownPush{
                        .DestExtent = dst.ValidExtent,
                        .SourceScaleUV = src.ScaleUV,
                        .SourceMaxUV = src.MaxUV,
                        .BrightPass = brightPass,
                        // The authored threshold is display-referred (1.0 = the post-exposure
                        // white point); dividing by the frame's resolved exposure moves it into
                        // HDR units, so it tracks the metered exposure across lighting regimes.
                        // The resolved exposure is the one cross-battery read (from the meter).
                        .Threshold =
                            view->BloomThreshold / std::max(meter->GetResolvedExposure(), 1e-5f),
                        .MaskTexture = maskHandle.Index,
                        .MaskSampler = maskSampler.Index,
                        .MaskEnabled = levelMask ? 1u : 0u,
                    });
                    cmd.Dispatch((dst.ValidExtent.x + 7) / 8, (dst.ValidExtent.y + 7) / 8, 1);
                });
        }

        // Up-sweep: from the coarsest finer level down to mip 0, dispatch k samples the
        // coarser mip k+1 (ShaderReadOnly) and read-modify-writes mip k (General) — two
        // subresources of one image in one pass. The pass declares both a Sample on mip k+1
        // and a StorageWrite on mip k; the StorageWrite orders it after the down-sweep that
        // wrote mip k (a General→General write-after-write barrier the graph derives), and a
        // per-level barrier before the next finer up-step reads mip k.
        for (u32 level = mipCount - 1; level-- > 0;)
        {
            RenderGraph::PassBuilder builder =
                graph.AddComputePass(fmt::format("Bloom Up Mip {}", level));
            builder.Sample(chainId.Level(level + 1));
            builder.StorageWrite(chainId.Level(level));

            const Ref<ComputePipeline> pipeline =
                m_Kernel == BloomKernel::Kawase ? m_UpKawasePipeline : m_UpPipeline;
            const Ref<DescriptorSet> set = m_UpSets[level];
            const u32 srcLevel = level + 1;
            builder.Execute(
                [pipeline, set, level, srcLevel, allocExtent](PassContext& inner)
                {
                    const auto* view = static_cast<const SceneView*>(inner.UserData());
                    VE_ASSERT(view != nullptr, "Bloom up pass: null SceneView");
                    const MipSubRect dst =
                        ComputeMipSubRect(view->RenderExtent, allocExtent, level);
                    const MipSubRect src =
                        ComputeMipSubRect(view->RenderExtent, allocExtent, srcLevel);
                    CommandBuffer& cmd = inner.Cmd();
                    cmd.BindPipeline(pipeline);
                    cmd.BindDescriptorSets(DescriptorSetBindInfo{
                        .Sets = {set},
                        .FirstSet = 3,
                        .PipelineBindPoint = PipelineBindPoint::Compute,
                    });
                    cmd.PushConstants(BloomUpPush{
                        .DestExtent = dst.ValidExtent,
                        .SourceScaleUV = src.ScaleUV,
                        .SourceMaxUV = src.MaxUV,
                        .Radius = view->BloomRadius,
                    });
                    cmd.Dispatch((dst.ValidExtent.x + 7) / 8, (dst.ValidExtent.y + 7) / 8, 1);
                });
        }

        // Composite over the valid sub-rect: result = hdr + mip0 * Intensity. Samples the HDR
        // target and bloom mip 0 (both at mip-0 sub-rect scale) and stores into the result the
        // terminal tonemap upscales.
        {
            RenderGraph::PassBuilder builder = graph.AddComputePass("Bloom Composite");
            builder.Sample(hdrId);
            builder.Sample(chainId.Level(0));
            builder.StorageWrite(resultId);

            const Ref<ComputePipeline> pipeline = m_CompositePipeline;
            const Ref<DescriptorSet> set = m_CompositeSet;
            builder.Execute(
                [pipeline, set, allocExtent](PassContext& inner)
                {
                    const auto* view = static_cast<const SceneView*>(inner.UserData());
                    VE_ASSERT(view != nullptr, "Bloom composite pass: null SceneView");
                    const MipSubRect r = ComputeMipSubRect(view->RenderExtent, allocExtent, 0);
                    CommandBuffer& cmd = inner.Cmd();
                    cmd.BindPipeline(pipeline);
                    cmd.BindDescriptorSets(DescriptorSetBindInfo{
                        .Sets = {set},
                        .FirstSet = 3,
                        .PipelineBindPoint = PipelineBindPoint::Compute,
                    });
                    cmd.PushConstants(BloomCompositePush{
                        .DestExtent = r.ValidExtent,
                        .SourceScaleUV = r.ScaleUV,
                        .SourceMaxUV = r.MaxUV,
                        .Intensity = view->BloomIntensity,
                    });
                    cmd.Dispatch((r.ValidExtent.x + 7) / 8, (r.ValidExtent.y + 7) / 8, 1);
                });
        }
    }
}
