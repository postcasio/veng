#include "SsrChain.h"

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
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/Sampler.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/SsrLimits.h>
#include <Veng/Renderer/Types.h>

#include "MipTail.h"

namespace Veng::Renderer
{
    namespace
    {
        // The fullscreen vertex stage shared by the trace and composite pipelines.
        constexpr AssetId FullscreenVertId{0xF46DD3C6F2AE0628ULL};
        constexpr AssetId SsrTraceFragId{0xBDBD7BC71B2B1E74ULL};
        constexpr AssetId SsrBlurDownCompId{0xEE0EED485023A7F6ULL};
        constexpr AssetId SsrCompositeFragId{0x50D9ECEAE45E31A1ULL};
        constexpr AssetId SsrHiZReduceCompId{0x93DA6E42B3B5479AULL};
        constexpr AssetId SsrHiZReduceTailCompId{0x7737782FD6CC04D1ULL};

        // Texels the min-Z tail dispatch's shared memory holds: one float each, within a 32 KiB
        // workgroup budget. Matches TAIL_CAPACITY in ssr_hiz_reduce_tail.comp.
        constexpr u32 MinZTailTexelCapacity = 7168;

        // The tail's first storage binding; one binding per tail slot follows (the hi-Z tail set
        // layout's).
        constexpr u32 MinZTailFirstStorageBinding = 1;

        // The min-Z tail push: the pyramid's level-0 extent, the tail's first level and its length.
        // Matches ssr_hiz_reduce_tail.comp (and the hi-Z tail layout's push range).
        struct MinZReduceTailPush
        {
            uvec2 Extent;
            u32 FirstLevel;
            u32 LevelCount;
        };

        // Linear float HDR format for the scene-color intermediate and reflection chain.
        constexpr Format HdrFormat = Format::RGBA16Sfloat;
        constexpr ImageUsage HdrUsage = ImageUsage::ColorAttachment | ImageUsage::Sampled;
        // The hit-distance chain: one confidence-premultiplied view-space length per texel.
        constexpr Format HitDistanceFormat = Format::R16Sfloat;
        // The surface chain: the device depth of the surface each texel's ray left from and its
        // square, each times the texel's coverage, and the coverage. Thirty-two bits, since
        // reverse-Z depth of distant geometry sits below half-float's normal range.
        constexpr Format SurfaceFormat = Format::RGBA32Sfloat;
        // Both mip chains: level 0 a trace target, deeper levels blur storage destinations.
        constexpr ImageUsage ChainUsage =
            ImageUsage::ColorAttachment | ImageUsage::Storage | ImageUsage::Sampled;
        // Single-channel float for the max-Z pyramid; under reverse-Z the reduction stores
        // the nearest surface, which is the max depth.
        constexpr Format HiZFormat = Format::R32Sfloat;

        // The mip chains run until their short axis reaches two texels; one level more is a single
        // row or column, across which the composite's B-spline has nothing to reconstruct.
        [[nodiscard]] u32 ChainMipCount(const uvec2 extent)
        {
            const u32 shortAxis = std::min(extent.x, extent.y);
            return shortAxis <= 1 ? 1u
                                  : std::max(1u, static_cast<u32>(std::bit_width(shortAxis)) - 1u);
        }

        // The blur's set 3 bindings, matching ssr_blur_down.comp: each chain's sampled source and
        // storage destination, and the shared sampler (the surface chain is loaded, not sampled).
        constexpr u32 BlurSourceBinding = 0;
        constexpr u32 BlurSamplerBinding = 1;
        constexpr u32 BlurDestBinding = 2;
        constexpr u32 BlurDistanceSourceBinding = 3;
        constexpr u32 BlurDistanceDestBinding = 4;
        constexpr u32 BlurSurfaceSourceBinding = 5;
        constexpr u32 BlurSurfaceDestBinding = 6;

        // The max-Z reduce push block, matching ssr_hiz_reduce.comp: the destination and source
        // mip extents, so a boundary invocation skips out-of-range texels and an odd parent
        // dimension folds its dropped row/column into the max.
        struct MinZReducePush
        {
            uvec2 DestExtent;
            uvec2 SourceExtent;
        };

        // The SSR trace push block, matching ssr_trace.frag PushConstants: the scene-color and
        // g-buffer bindless slots, the shared sampler, the view-constants region, the reflection
        // extent, and the ray parameters (max distance, hit-tolerance floor, roughness cutoff, and the
        // per-ray depth-test and hi-Z probe budget).
        struct SsrTracePush
        {
            u32 SceneColorTexture;
            u32 DepthTexture;
            u32 NormalTexture;
            u32 OrmTexture;
            u32 HiZTexture;
            u32 Sampler;
            u32 ViewConstantsIndex;
            u32 HiZLevels;
            uvec2 Extent;
            f32 MaxDistance;
            f32 Thickness;
            f32 MaxRoughness;
            u32 MaxSteps;
        };

        // The SSR reflection blur-downsample push, matching ssr_blur_down.comp: the destination
        // mip extent.
        struct SsrBlurPush
        {
            uvec2 DestExtent;
        };

        // The SSR composite push, matching ssr_composite.frag PushConstants: the scene-color,
        // reflection, hit-distance and g-buffer slots, the shared + chain samplers, the
        // view-constants region, the reflection mix, the roughness cutoff it fades toward, the
        // chains' level count, and the surface chain the coarse levels are weighted by.
        struct SsrCompositePush
        {
            u32 SceneColorTexture;
            u32 ReflectionTexture;
            u32 HitDistanceTexture;
            u32 AlbedoTexture;
            u32 NormalTexture;
            u32 OrmTexture;
            u32 DepthTexture;
            u32 Sampler;
            u32 ReflectionSampler;
            u32 ViewConstantsIndex;
            f32 Intensity;
            f32 MaxRoughness;
            u32 MipCount;
            u32 SurfaceTexture;
            u32 Pad0;
            u32 Pad1;
        };
    }

    Unique<SsrChain> SsrChain::Create(Context& context, AssetManager& assets,
                                      const Ref<PipelineLayout>& hiZReduceLayout,
                                      const Ref<PipelineLayout>& hiZTailLayout)
    {
        return Unique<SsrChain>(new SsrChain(context, assets, hiZReduceLayout, hiZTailLayout));
    }

    SsrChain::SsrChain(Context& context, AssetManager& assets,
                       const Ref<PipelineLayout>& hiZReduceLayout,
                       const Ref<PipelineLayout>& hiZTailLayout)
        : m_Context(context)
    {
        const auto LoadShader = [&](const AssetId id, const char* what) -> AssetHandle<Veng::Shader>
        {
            const AssetResult<AssetHandle<Veng::Shader>> result = assets.LoadSync<Veng::Shader>(id);
            VE_ASSERT(result.has_value(), "SsrChain: {} shader load failed: {}", what,
                      result.error().Detail);
            return *result;
        };

        const AssetHandle<Veng::Shader> vs = LoadShader(FullscreenVertId, "fullscreen vertex");
        const AssetHandle<Veng::Shader> ssrTraceFs =
            LoadShader(SsrTraceFragId, "SSR trace fragment");
        const AssetHandle<Veng::Shader> ssrCompositeFs =
            LoadShader(SsrCompositeFragId, "SSR composite fragment");
        const AssetHandle<Veng::Shader> ssrBlurCs =
            LoadShader(SsrBlurDownCompId, "SSR blur downsample");
        const AssetHandle<Veng::Shader> ssrHiZReduceCs =
            LoadShader(SsrHiZReduceCompId, "SSR max-Z reduce");
        const AssetHandle<Veng::Shader> ssrHiZTailCs =
            LoadShader(SsrHiZReduceTailCompId, "SSR max-Z reduce tail");

        // Builds a fullscreen pipeline (shared vertex stage) over a layout, naming the color formats.
        const auto MakePipeline =
            [&](const char* name, const Ref<PipelineLayout>& layout,
                const AssetHandle<Veng::Shader>& fs,
                vector<PipelineAttachmentInfo> attachments) -> Ref<GraphicsPipeline>
        {
            return GraphicsPipeline::Create(
                m_Context, {
                               .Name = name,
                               .ColorAttachments = std::move(attachments),
                               .PipelineLayout = layout,
                               .ShaderStages =
                                   {
                                       {.Stage = ShaderStage::Vertex, .Module = vs.Get()->Module},
                                       {.Stage = ShaderStage::Fragment, .Module = fs.Get()->Module},
                                   },
                           });
        };

        // The max-Z reduction reuses the hi-Z reduce layout/set layout (sampled source + storage
        // dest + the reduce push) — only the reduce operator (max vs min) differs.
        m_HiZReducePipeline = ComputePipeline::Create(
            m_Context, {
                           .Name = "SceneRenderer SSR MinZ Reduce Pipeline",
                           .PipelineLayout = hiZReduceLayout,
                           .ShaderStage = {.Stage = ShaderStage::Compute,
                                           .Module = ssrHiZReduceCs.Get()->Module},
                       });
        // Its coarse tail likewise reuses the hi-Z tail layout (the source level sampled, a storage
        // view per tail slot, the tail push); only the reduce operator and footprint differ.
        m_HiZTailPipeline = ComputePipeline::Create(
            m_Context, {
                           .Name = "SceneRenderer SSR MinZ Tail Pipeline",
                           .PipelineLayout = hiZTailLayout,
                           .ShaderStage = {.Stage = ShaderStage::Compute,
                                           .Module = ssrHiZTailCs.Get()->Module},
                       });

        m_TraceLayout = PipelineLayout::Create(
            m_Context,
            {
                .Name = "SceneRenderer SSR Trace Layout",
                .PushConstantRanges = {PushConstantRange::Of<SsrTracePush>(ShaderStage::Fragment)},
            });
        m_TracePipeline = MakePipeline(
            "SceneRenderer SSR Trace Pipeline", m_TraceLayout, ssrTraceFs,
            {{.Format = HdrFormat}, {.Format = HitDistanceFormat}, {.Format = SurfaceFormat}});

        m_CompositeLayout = PipelineLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer SSR Composite Layout",
                           .PushConstantRanges = {PushConstantRange::Of<SsrCompositePush>(
                               ShaderStage::Fragment)},
                       });
        m_CompositePipeline =
            MakePipeline("SceneRenderer SSR Composite Pipeline", m_CompositeLayout, ssrCompositeFs,
                         {{.Format = HdrFormat}});

        const auto ComputeBinding = [](const u32 binding, const DescriptorType type)
        {
            return DescriptorBinding{
                .Binding = binding, .Type = type, .Count = 1, .Stages = ShaderStage::Compute};
        };
        m_BlurSetLayout = DescriptorSetLayout::Create(
            m_Context,
            {
                .Name = "SceneRenderer SSR Blur Set Layout",
                .Bindings =
                    {
                        ComputeBinding(BlurSourceBinding, DescriptorType::SampledImage),
                        ComputeBinding(BlurSamplerBinding, DescriptorType::Sampler),
                        ComputeBinding(BlurDestBinding, DescriptorType::StorageImage),
                        ComputeBinding(BlurDistanceSourceBinding, DescriptorType::SampledImage),
                        ComputeBinding(BlurDistanceDestBinding, DescriptorType::StorageImage),
                        ComputeBinding(BlurSurfaceSourceBinding, DescriptorType::SampledImage),
                        ComputeBinding(BlurSurfaceDestBinding, DescriptorType::StorageImage),
                    },
            });
        m_BlurLayout = PipelineLayout::Create(
            m_Context,
            {
                .Name = "SceneRenderer SSR Blur Layout",
                .DescriptorSetLayouts = {m_BlurSetLayout},
                .PushConstantRanges = {PushConstantRange::Of<SsrBlurPush>(ShaderStage::Compute)},
            });
        m_BlurPipeline = ComputePipeline::Create(
            m_Context,
            {
                .Name = "SceneRenderer SSR Blur Pipeline",
                .PipelineLayout = m_BlurLayout,
                .ShaderStage = {.Stage = ShaderStage::Compute, .Module = ssrBlurCs.Get()->Module},
            });
    }

    SsrChain::~SsrChain()
    {
        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_SceneHandle);
        bindless.Release(m_ReflectionSampleHandle);
        bindless.Release(m_HitDistanceSampleHandle);
        bindless.Release(m_SurfaceSampleHandle);
        bindless.Release(m_HiZSampleHandle);
    }

    uvec2 SsrChain::RenderExtent() const
    {
        if (m_ResolutionScale == static_cast<u8>(SceneRendererSettings::SsrResolution::Half))
        {
            return glm::max(uvec2(1), m_Extent / 2u);
        }
        if (m_ResolutionScale == static_cast<u8>(SceneRendererSettings::SsrResolution::Quarter))
        {
            return glm::max(uvec2(1), m_Extent / 4u);
        }
        return m_Extent;
    }

    void SsrChain::Recreate(const SceneRendererSettings& settings, const uvec2 extent,
                            const Ref<ImageView>& depthView,
                            const Ref<DescriptorSetLayout>& hiZReduceSetLayout,
                            const Ref<DescriptorSetLayout>& hiZTailSetLayout)
    {
        m_Extent = extent;
        m_ResolutionScale = static_cast<u8>(settings.SsrResolutionScale);

        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_SceneHandle);
        bindless.Release(m_ReflectionSampleHandle);
        bindless.Release(m_HitDistanceSampleHandle);
        bindless.Release(m_SurfaceSampleHandle);
        bindless.Release(m_HiZSampleHandle);
        m_SceneHandle = {};
        m_ReflectionSampleHandle = {};
        m_HitDistanceSampleHandle = {};
        m_SurfaceSampleHandle = {};
        m_HiZSampleHandle = {};

        // SSR targets exist only when SSR runs (the toggle or the Reflections debug arm).
        const bool ssrWanted = settings.SSR || settings.Mode == DebugView::Reflections;
        if (!ssrWanted)
        {
            m_SceneImage.reset();
            m_SceneView.reset();
            m_ReflectionImage.reset();
            m_ReflectionMips.clear();
            m_ReflectionSampleView.reset();
            m_HitDistanceImage.reset();
            m_HitDistanceMips.clear();
            m_HitDistanceSampleView.reset();
            m_SurfaceImage.reset();
            m_SurfaceMips.clear();
            m_SurfaceSampleView.reset();
            m_ReflectionSampler.reset();
            m_BlurSets.clear();
            m_HiZImage.reset();
            m_HiZMips.clear();
            m_HiZSampleView.reset();
            m_HiZReduceSets.clear();
            m_HiZTailSet.reset();
            return;
        }

        // The lit scene color SSR reads: lighting/TAA writes here, the composite reflects into
        // it and writes the HDR target, so the bloom/tonemap tail is unchanged.
        m_SceneImage = Image::Create(m_Context, {
                                                    .Name = "SceneRenderer SSR Scene",
                                                    .Extent = {m_Extent.x, m_Extent.y, 1},
                                                    .Format = HdrFormat,
                                                    .Usage = HdrUsage,
                                                });
        m_SceneView = ImageView::Create(
            m_Context, {.Name = "SceneRenderer SSR Scene View", .Image = m_SceneImage});
        m_SceneHandle = bindless.Register(m_SceneView);

        // The trace, max-Z pyramid, and blur chain run at the SSR resolution (the scene color
        // above stays full-res: the trace samples it by reflected UV, the composite by logical UV).
        const uvec2 ssrExtent = RenderExtent();

        // The two mip chains, the reflected radiance and the hit distance: mip 0 the trace writes,
        // coarser mips the blur produces.
        const u32 mipCount = ChainMipCount(ssrExtent);
        const auto CreateChain = [&](const char* name, const Format format,
                                     std::vector<Ref<ImageView>>& mips) -> Ref<Image>
        {
            Ref<Image> image =
                Image::Create(m_Context, {
                                             .Name = fmt::format("SceneRenderer SSR {}", name),
                                             .Extent = {ssrExtent.x, ssrExtent.y, 1},
                                             .MipLevels = mipCount,
                                             .Format = format,
                                             .Usage = ChainUsage,
                                         });
            mips.clear();
            mips.reserve(mipCount);
            for (u32 level = 0; level < mipCount; level++)
            {
                mips.push_back(ImageView::Create(
                    m_Context,
                    {
                        .Name = fmt::format("SceneRenderer SSR {} Mip {} View", name, level),
                        .Image = image,
                        .BaseMipLevel = level,
                        .MipLevels = 1,
                    }));
            }
            return image;
        };
        m_ReflectionImage = CreateChain("Reflection", HdrFormat, m_ReflectionMips);
        m_ReflectionSampleView =
            ImageView::Create(m_Context, {
                                             .Name = "SceneRenderer SSR Reflection Sample View",
                                             .Image = m_ReflectionImage,
                                             .MipLevels = mipCount,
                                         });
        m_ReflectionSampleHandle = bindless.Register(m_ReflectionSampleView);
        m_HitDistanceImage = CreateChain("Hit Distance", HitDistanceFormat, m_HitDistanceMips);
        m_HitDistanceSampleView =
            ImageView::Create(m_Context, {
                                             .Name = "SceneRenderer SSR Hit Distance Sample View",
                                             .Image = m_HitDistanceImage,
                                             .MipLevels = mipCount,
                                         });
        m_HitDistanceSampleHandle = bindless.Register(m_HitDistanceSampleView);
        m_SurfaceImage = CreateChain("Surface", SurfaceFormat, m_SurfaceMips);
        m_SurfaceSampleView =
            ImageView::Create(m_Context, {
                                             .Name = "SceneRenderer SSR Surface Sample View",
                                             .Image = m_SurfaceImage,
                                             .MipLevels = mipCount,
                                         });
        m_SurfaceSampleHandle = bindless.Register(m_SurfaceSampleView);

        // Linear within a level for the blur's and the composite's bilinear taps; the composite
        // names every level it reads. The sample views carry the chains' level count and bound the
        // LOD, so the description names no clamp of its own and stays the same across every extent.
        const SharedSampler shared = bindless.AcquireSampler({
            .Name = "SceneRenderer SSR Reflection Sampler",
            .MagFilter = Filter::Linear,
            .MinFilter = Filter::Linear,
            .MipmapMode = MipmapMode::Linear,
            .AddressModeU = AddressMode::ClampToEdge,
            .AddressModeV = AddressMode::ClampToEdge,
            .AddressModeW = AddressMode::ClampToEdge,
            .AnisotropyEnabled = false,
            .MaxLod = LodClampNone,
        });
        m_ReflectionSampler = shared.Sampler;
        m_ReflectionSamplerHandle = shared.Handle;

        // Per-level blur sets: set k reads both chains' mip k-1 and writes their mip k; index 0
        // produces mip 1.
        m_BlurSets.clear();
        if (mipCount > 1)
        {
            m_BlurSets.reserve(mipCount - 1);
            for (u32 level = 1; level < mipCount; level++)
            {
                Ref<DescriptorSet> set = DescriptorSet::Create(
                    m_Context, {
                                   .Name = fmt::format("SceneRenderer SSR Blur Set {}", level),
                                   .Layout = m_BlurSetLayout,
                               });
                set->Write(BlurSourceBinding, m_ReflectionMips[level - 1]);
                set->Write(BlurSamplerBinding, m_ReflectionSampler);
                set->Write(BlurDestBinding, m_ReflectionMips[level]);
                set->Write(BlurDistanceSourceBinding, m_HitDistanceMips[level - 1]);
                set->Write(BlurDistanceDestBinding, m_HitDistanceMips[level]);
                set->Write(BlurSurfaceSourceBinding, m_SurfaceMips[level - 1]);
                set->Write(BlurSurfaceDestBinding, m_SurfaceMips[level]);
                m_BlurSets.push_back(std::move(set));
            }
        }

        // Max-Z depth pyramid (mirrors the hi-Z reduction, opposite reduction): a full mip chain
        // the trace marches through to skip empty space. Reduced from this frame's depth before the
        // trace; distinct from the occlusion-culling min-Z pyramid.
        const u32 maxDim = std::max(ssrExtent.x, ssrExtent.y);
        const u32 hizMips = maxDim == 0 ? 1u : std::bit_width(maxDim);
        m_HiZImage =
            Image::Create(m_Context, {
                                         .Name = "SceneRenderer SSR MinZ",
                                         .Extent = {ssrExtent.x, ssrExtent.y, 1},
                                         .MipLevels = hizMips,
                                         .Format = HiZFormat,
                                         .Usage = ImageUsage::Storage | ImageUsage::Sampled,
                                     });
        m_HiZMips.clear();
        m_HiZMips.reserve(hizMips);
        for (u32 level = 0; level < hizMips; level++)
        {
            m_HiZMips.push_back(ImageView::Create(
                m_Context, {
                               .Name = fmt::format("SceneRenderer SSR MinZ Mip {} View", level),
                               .Image = m_HiZImage,
                               .BaseMipLevel = level,
                               .MipLevels = 1,
                           }));
        }
        m_HiZSampleView =
            ImageView::Create(m_Context, {
                                             .Name = "SceneRenderer SSR MinZ Sample View",
                                             .Image = m_HiZImage,
                                             .MipLevels = hizMips,
                                         });
        m_HiZSampleHandle = bindless.Register(m_HiZSampleView);

        // Per-mip reduction sets (the hi-Z reduce set layout): set k binds mip k's source (the
        // depth target for k=0, max-Z mip k-1 otherwise) and mip k's destination storage view.
        m_HiZReduceSets.clear();
        m_HiZReduceSets.reserve(hizMips);
        for (u32 level = 0; level < hizMips; level++)
        {
            Ref<DescriptorSet> set = DescriptorSet::Create(
                m_Context, {
                               .Name = fmt::format("SceneRenderer SSR MinZ Reduce Set {}", level),
                               .Layout = hiZReduceSetLayout,
                           });
            const Ref<ImageView>& source = level == 0 ? depthView : m_HiZMips[level - 1];
            set->Write(0, source);
            set->Write(1, m_HiZMips[level]);
            m_HiZReduceSets.push_back(std::move(set));
        }

        // The coarse tail reads the level before it and writes each of its levels; the slots past
        // its length repeat its last level, bound but never written. Level 0 is the full-resolution
        // ingest, a different footprint, so the tail starts no earlier than level 1.
        m_HiZTailFirst = MipTailFirstLevel(ssrExtent, hizMips, 1, MinZTailTexelCapacity);
        m_HiZTailSet.reset();
        if (m_HiZTailFirst < hizMips)
        {
            m_HiZTailSet = DescriptorSet::Create(
                m_Context, {.Name = "SceneRenderer SSR MinZ Tail Set", .Layout = hiZTailSetLayout});
            m_HiZTailSet->Write(0, m_HiZMips[m_HiZTailFirst - 1]);
            for (u32 slot = 0; slot < MipTailMaxLevels; slot++)
            {
                m_HiZTailSet->Write(MinZTailFirstStorageBinding + slot,
                                    m_HiZMips[std::min(m_HiZTailFirst + slot, hizMips - 1)]);
            }
        }
    }

    void SsrChain::Declare(RenderGraph& graph, const ResourceId sceneId,
                           const MipChainId reflectionChainId, const MipChainId hiZChainId,
                           const ResourceId albedoId, const ResourceId normalId,
                           const ResourceId ormId, const ResourceId depthId, const ResourceId hdrId,
                           const TextureHandle depthHandle, const TextureHandle normalHandle,
                           const TextureHandle ormHandle, const TextureHandle albedoHandle,
                           const SamplerHandle samplerHandle)
    {
        const u32 mipCount = static_cast<u32>(m_ReflectionMips.size());
        const u32 hizMips = static_cast<u32>(m_HiZMips.size());
        const uvec2 ssrExtent = RenderExtent();
        m_HitDistanceChainId = graph.ImportImageMips("SceneRenderer SSR Hit Distance", mipCount);
        const MipChainId distanceChainId = m_HitDistanceChainId;
        m_SurfaceChainId = graph.ImportImageMips("SceneRenderer SSR Surface", mipCount);
        const MipChainId surfaceChainId = m_SurfaceChainId;

        // Max-Z reduction: build this frame's closest-surface pyramid from the depth target
        // before the trace. One dispatch per mip above the coarse tail, and one for the tail; mip 0
        // ingests the full-res depth target into the SSR-resolution pyramid (a downsample when SSR
        // runs below full res, a 1:1 copy at Full), deeper mips halve the prior. The per-mip graph
        // surface derives the read-after-write barriers; the trace then reads the whole chain.
        const u32 hizTailFirst = m_HiZTailSet != nullptr ? m_HiZTailFirst : hizMips;
        for (u32 level = 0; level < hizTailFirst; level++)
        {
            const u32 dstW = std::max(ssrExtent.x >> level, 1u);
            const u32 dstH = std::max(ssrExtent.y >> level, 1u);
            const u32 srcW = level == 0 ? m_Extent.x : std::max(ssrExtent.x >> (level - 1), 1u);
            const u32 srcH = level == 0 ? m_Extent.y : std::max(ssrExtent.y >> (level - 1), 1u);

            RenderGraph::PassBuilder builder =
                graph.AddComputePass(fmt::format("SSR MinZ Reduce Mip {}", level));
            if (level == 0)
            {
                builder.Sample(depthId);
            }
            else
            {
                builder.Sample(hiZChainId.Level(level - 1));
            }
            builder.StorageWrite(hiZChainId.Level(level));

            const Ref<ComputePipeline> pipeline = m_HiZReducePipeline;
            const Ref<DescriptorSet> set = m_HiZReduceSets[level];
            const MinZReducePush push{
                .DestExtent = {dstW, dstH},
                .SourceExtent = {srcW, srcH},
            };
            builder.Execute(
                [pipeline, set, push](PassContext& inner)
                {
                    CommandBuffer& cmd = inner.Cmd();
                    cmd.BindPipeline(pipeline);
                    cmd.BindDescriptorSets(DescriptorSetBindInfo{
                        .Sets = {set},
                        .FirstSet = 3, // sets 0-2 are the typed bindless registries
                        .PipelineBindPoint = PipelineBindPoint::Compute,
                    });
                    cmd.PushConstants(push);
                    cmd.Dispatch((push.DestExtent.x + 7) / 8, (push.DestExtent.y + 7) / 8, 1);
                });
        }
        if (hizTailFirst < hizMips)
        {
            RenderGraph::PassBuilder tail = graph.AddComputePass(
                fmt::format("SSR MinZ Reduce Tail Mips {}-{}", hizTailFirst, hizMips - 1));
            tail.Sample(hiZChainId.Level(hizTailFirst - 1));
            for (u32 level = hizTailFirst; level < hizMips; level++)
            {
                tail.StorageWrite(hiZChainId.Level(level));
            }
            const Ref<ComputePipeline> pipeline = m_HiZTailPipeline;
            const Ref<DescriptorSet> set = m_HiZTailSet;
            const MinZReduceTailPush push{
                .Extent = ssrExtent,
                .FirstLevel = hizTailFirst,
                .LevelCount = hizMips - hizTailFirst,
            };
            tail.Execute(
                [pipeline, set, push](PassContext& inner)
                {
                    CommandBuffer& cmd = inner.Cmd();
                    cmd.BindPipeline(pipeline);
                    cmd.BindDescriptorSets(DescriptorSetBindInfo{
                        .Sets = {set},
                        .FirstSet = 3,
                        .PipelineBindPoint = PipelineBindPoint::Compute,
                    });
                    cmd.PushConstants(push);
                    cmd.Dispatch(1, 1, 1);
                });
        }

        // Trace: reflect the view vector about the g-buffer normal, march the depth buffer, and
        // write the reflected scene radiance and the hit distance, premultiplied by the hit's
        // confidence, into both chains' mip 0.
        {
            const TextureHandle sceneHandle = m_SceneHandle;
            const TextureHandle hizHandle = m_HiZSampleHandle;

            RenderGraph::PassBuilder traceBuilder = graph.AddPass("SSR Trace");
            traceBuilder
                .Color({
                    .Resource = reflectionChainId.Level(0),
                    .Load = LoadOp::Clear,
                    .Store = StoreOp::Store,
                    .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 0.0f},
                })
                .Color({
                    .Resource = distanceChainId.Level(0),
                    .Load = LoadOp::Clear,
                    .Store = StoreOp::Store,
                    .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 0.0f},
                })
                .Color({
                    .Resource = surfaceChainId.Level(0),
                    .Load = LoadOp::Clear,
                    .Store = StoreOp::Store,
                    .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 0.0f},
                })
                .Sample(sceneId)
                .Sample(normalId)
                .Sample(ormId)
                .Sample(depthId);
            // The trace Loads the whole max-Z chain by bindless handle; declaring each mip
            // sampled drives the graph-derived General → ShaderReadOnly transition after the
            // reduction wrote it.
            for (u32 level = 0; level < hizMips; level++)
            {
                traceBuilder.Sample(hiZChainId.Level(level));
            }
            traceBuilder.Execute(
                [this, sceneHandle, depthHandle, normalHandle, ormHandle, hizHandle, hizMips,
                 samplerHandle, ssrExtent](PassContext& inner)
                {
                    CommandBuffer& cmd = inner.Cmd();
                    const BindlessRegistry& registry = m_Context.GetBindlessRegistry();
                    const auto* viewPtr = static_cast<const SceneView*>(inner.UserData());
                    VE_ASSERT(viewPtr != nullptr, "SSR trace pass: null SceneView");
                    const SceneView& view = *viewPtr;
                    // The trace writes the SSR-resolution reflection target, so it marches and
                    // steps at the SSR extent; the g-buffer it reads stays full-res, sampled by
                    // logical UV. SSR disables the dynamic-resolution sub-rect, so RenderExtent
                    // is the full extent the SSR extent derives from.
                    const uvec2 renderExtent = ssrExtent;

                    cmd.BindPipeline(m_TracePipeline);
                    cmd.SetViewport({0, 0}, renderExtent);
                    cmd.SetScissor({0, 0}, renderExtent);
                    registry.Bind(cmd);
                    cmd.PushConstants(SsrTracePush{
                        .SceneColorTexture = sceneHandle.Index,
                        .DepthTexture = depthHandle.Index,
                        .NormalTexture = normalHandle.Index,
                        .OrmTexture = ormHandle.Index,
                        .HiZTexture = hizHandle.Index,
                        .Sampler = samplerHandle.Index,
                        .ViewConstantsIndex = registry.GetCurrentViewConstantsIndex(),
                        .HiZLevels = hizMips,
                        .Extent = renderExtent,
                        .MaxDistance = std::max(view.SsrMaxDistance, 0.0f),
                        .Thickness = std::max(view.SsrThickness, 0.0f),
                        .MaxRoughness = view.SsrMaxRoughness,
                        .MaxSteps = ClampSsrMaxSteps(view.SsrMaxSteps),
                    });
                    cmd.DrawFullscreenTriangle();
                });
        }

        // Blur down-sweep: low-pass both chains a level at a time (mip k filters mip k-1). The
        // per-mip graph surface derives the read-after-write barrier between writing mip k and
        // reading it.
        const uvec2 allocExtent = ssrExtent;
        for (u32 level = 1; level < mipCount; level++)
        {
            const u32 dstW = std::max(allocExtent.x >> level, 1u);
            const u32 dstH = std::max(allocExtent.y >> level, 1u);
            const Ref<ComputePipeline> pipeline = m_BlurPipeline;
            const Ref<DescriptorSet> set = m_BlurSets[level - 1];

            graph.AddComputePass(fmt::format("SSR Blur Mip {}", level))
                .Sample(reflectionChainId.Level(level - 1))
                .Sample(distanceChainId.Level(level - 1))
                .Sample(surfaceChainId.Level(level - 1))
                .StorageWrite(reflectionChainId.Level(level))
                .StorageWrite(distanceChainId.Level(level))
                .StorageWrite(surfaceChainId.Level(level))
                .Execute(
                    [pipeline, set, dstW, dstH](PassContext& inner)
                    {
                        CommandBuffer& cmd = inner.Cmd();
                        cmd.BindPipeline(pipeline);
                        cmd.BindDescriptorSets(DescriptorSetBindInfo{
                            .Sets = {set},
                            .FirstSet = 3, // sets 0-2 are the typed bindless registries
                            .PipelineBindPoint = PipelineBindPoint::Compute,
                        });
                        cmd.PushConstants(SsrBlurPush{.DestExtent = {dstW, dstH}});
                        cmd.Dispatch((dstW + 7) / 8, (dstH + 7) / 8, 1);
                    });
        }

        // Composite: fold the reflection, blurred by its cone's footprint, into the scene color and
        // write the HDR target the bloom/tonemap tail reads.
        {
            const TextureHandle sceneHandle = m_SceneHandle;
            const TextureHandle reflectionHandle = m_ReflectionSampleHandle;
            const TextureHandle distanceHandle = m_HitDistanceSampleHandle;
            const TextureHandle surfaceHandle = m_SurfaceSampleHandle;
            const SamplerHandle reflSamplerHandle = m_ReflectionSamplerHandle;

            RenderGraph::PassBuilder builder = graph.AddPass("SSR Composite");
            builder
                .Color({
                    .Resource = hdrId,
                    .Load = LoadOp::Clear,
                    .Store = StoreOp::Store,
                    .Clear = ClearColor{.R = 0.0f, .G = 0.0f, .B = 0.0f, .A = 1.0f},
                })
                .Sample(sceneId)
                .Sample(albedoId)
                .Sample(normalId)
                .Sample(ormId)
                .Sample(depthId);
            // Every level of either chain the composite may read must be ShaderReadOnly.
            for (u32 level = 0; level < mipCount; level++)
            {
                builder.Sample(reflectionChainId.Level(level));
                builder.Sample(distanceChainId.Level(level));
                builder.Sample(surfaceChainId.Level(level));
            }

            builder.Execute(
                [this, sceneHandle, reflectionHandle, distanceHandle, surfaceHandle, albedoHandle,
                 normalHandle, ormHandle, depthHandle, samplerHandle, reflSamplerHandle,
                 mipCount](PassContext& inner)
                {
                    CommandBuffer& cmd = inner.Cmd();
                    const BindlessRegistry& registry = m_Context.GetBindlessRegistry();
                    const auto* viewPtr = static_cast<const SceneView*>(inner.UserData());
                    VE_ASSERT(viewPtr != nullptr, "SSR composite pass: null SceneView");
                    const SceneView& view = *viewPtr;
                    const uvec2 renderExtent = view.RenderExtent;

                    cmd.BindPipeline(m_CompositePipeline);
                    cmd.SetViewport({0, 0}, renderExtent);
                    cmd.SetScissor({0, 0}, renderExtent);
                    registry.Bind(cmd);
                    cmd.PushConstants(SsrCompositePush{
                        .SceneColorTexture = sceneHandle.Index,
                        .ReflectionTexture = reflectionHandle.Index,
                        .HitDistanceTexture = distanceHandle.Index,
                        .AlbedoTexture = albedoHandle.Index,
                        .NormalTexture = normalHandle.Index,
                        .OrmTexture = ormHandle.Index,
                        .DepthTexture = depthHandle.Index,
                        .Sampler = samplerHandle.Index,
                        .ReflectionSampler = reflSamplerHandle.Index,
                        .ViewConstantsIndex = registry.GetCurrentViewConstantsIndex(),
                        .Intensity = view.SsrIntensity,
                        .MaxRoughness = view.SsrMaxRoughness,
                        .MipCount = mipCount,
                        .SurfaceTexture = surfaceHandle.Index,
                    });
                    cmd.DrawFullscreenTriangle();
                });
        }
    }
}
