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
#include <Veng/Renderer/Types.h>

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

        // Linear float HDR format for the scene-color intermediate and reflection chain.
        constexpr Format HdrFormat = Format::RGBA16Sfloat;
        constexpr ImageUsage HdrUsage = ImageUsage::ColorAttachment | ImageUsage::Sampled;
        // Single-channel float for the max-Z pyramid; under reverse-Z the reduction stores
        // the nearest surface, which is the max depth.
        constexpr Format HiZFormat = Format::R32Sfloat;

        // The reflection mip chain stops this many levels short of 1×1 — a rough reflection needs
        // no 1-px mip.
        constexpr u32 SsrReflectionTileShift = 3;

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
        // extent, and the ray parameters (max distance, hit thickness, roughness cutoff, step count).
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
        // reflection, and g-buffer slots, the shared + reflection samplers, the view-constants
        // region, the reflection mix, and the reflection mip count (roughness → LOD).
        struct SsrCompositePush
        {
            u32 SceneColorTexture;
            u32 ReflectionTexture;
            u32 AlbedoTexture;
            u32 NormalTexture;
            u32 OrmTexture;
            u32 DepthTexture;
            u32 Sampler;
            u32 ReflectionSampler;
            u32 ViewConstantsIndex;
            f32 Intensity;
            u32 MipCount;
            u32 Pad0;
        };
    }

    Unique<SsrChain> SsrChain::Create(Context& context, AssetManager& assets,
                                      const Ref<PipelineLayout>& hiZReduceLayout,
                                      const Ref<DescriptorSetLayout>& bloomDownUpLayout)
    {
        return Unique<SsrChain>(new SsrChain(context, assets, hiZReduceLayout, bloomDownUpLayout));
    }

    SsrChain::SsrChain(Context& context, AssetManager& assets,
                       const Ref<PipelineLayout>& hiZReduceLayout,
                       const Ref<DescriptorSetLayout>& bloomDownUpLayout)
        : m_Context(context)
    {
        auto LoadShader = [&](const AssetId id, const char* what) -> AssetHandle<Veng::Shader>
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

        // Builds a fullscreen pipeline (shared vertex stage) over a layout, naming the color format.
        auto MakePipeline = [&](const char* name, const Ref<PipelineLayout>& layout,
                                const AssetHandle<Veng::Shader>& fs,
                                const Format format) -> Ref<GraphicsPipeline>
        {
            return GraphicsPipeline::Create(
                m_Context, {
                               .Name = name,
                               .ColorAttachments = {{.Format = format}},
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

        m_TraceLayout = PipelineLayout::Create(
            m_Context,
            {
                .Name = "SceneRenderer SSR Trace Layout",
                .PushConstantRanges = {PushConstantRange::Of<SsrTracePush>(ShaderStage::Fragment)},
            });
        m_TracePipeline =
            MakePipeline("SceneRenderer SSR Trace Pipeline", m_TraceLayout, ssrTraceFs, HdrFormat);

        m_CompositeLayout = PipelineLayout::Create(
            m_Context, {
                           .Name = "SceneRenderer SSR Composite Layout",
                           .PushConstantRanges = {PushConstantRange::Of<SsrCompositePush>(
                               ShaderStage::Fragment)},
                       });
        m_CompositePipeline = MakePipeline("SceneRenderer SSR Composite Pipeline",
                                           m_CompositeLayout, ssrCompositeFs, HdrFormat);

        m_BlurLayout = PipelineLayout::Create(
            m_Context,
            {
                .Name = "SceneRenderer SSR Blur Layout",
                .DescriptorSetLayouts = {bloomDownUpLayout},
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
                            const Ref<DescriptorSetLayout>& bloomDownUpLayout)
    {
        m_Extent = extent;
        m_ResolutionScale = static_cast<u8>(settings.SsrResolutionScale);

        BindlessRegistry& bindless = m_Context.GetBindlessRegistry();
        bindless.Release(m_SceneHandle);
        bindless.Release(m_ReflectionSampleHandle);
        bindless.Release(m_HiZSampleHandle);
        m_SceneHandle = {};
        m_ReflectionSampleHandle = {};
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
            m_ReflectionSampler.reset();
            m_BlurSets.clear();
            m_HiZImage.reset();
            m_HiZMips.clear();
            m_HiZSampleView.reset();
            m_HiZReduceSets.clear();
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

        // The reflection mip chain: mip 0 the trace writes, coarser mips the blur produces. The
        // chain stops SsrReflectionTileShift levels short of 1×1 (a rough reflection needs no 1-px mip).
        const u32 maxDim = std::max(ssrExtent.x, ssrExtent.y);
        const u32 mipCount =
            maxDim == 0 ? 1u : std::max(1u, std::bit_width(maxDim) - SsrReflectionTileShift);

        m_ReflectionImage =
            Image::Create(m_Context, {
                                         .Name = "SceneRenderer SSR Reflection",
                                         .Extent = {ssrExtent.x, ssrExtent.y, 1},
                                         .MipLevels = mipCount,
                                         .Format = HdrFormat,
                                         .Usage = ImageUsage::ColorAttachment |
                                                  ImageUsage::Storage | ImageUsage::Sampled,
                                     });

        m_ReflectionMips.clear();
        m_ReflectionMips.reserve(mipCount);
        for (u32 level = 0; level < mipCount; level++)
        {
            m_ReflectionMips.push_back(ImageView::Create(
                m_Context,
                {
                    .Name = fmt::format("SceneRenderer SSR Reflection Mip {} View", level),
                    .Image = m_ReflectionImage,
                    .BaseMipLevel = level,
                    .MipLevels = 1,
                }));
        }
        m_ReflectionSampleView =
            ImageView::Create(m_Context, {
                                             .Name = "SceneRenderer SSR Reflection Sample View",
                                             .Image = m_ReflectionImage,
                                             .MipLevels = mipCount,
                                         });
        m_ReflectionSampleHandle = bindless.Register(m_ReflectionSampleView);

        // Trilinear over the chain so the composite's roughness LOD blends between mips smoothly.
        // The sample view carries the chain's level count and is what bounds the LOD, so the
        // description names no clamp of its own and stays the same across every extent.
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

        // Per-level blur sets (the bloom down/up set layout: sampled source + sampler + storage
        // dest). Set k reads mip k-1 and writes mip k; index 0 produces mip 1.
        m_BlurSets.clear();
        if (mipCount > 1)
        {
            m_BlurSets.reserve(mipCount - 1);
            for (u32 level = 1; level < mipCount; level++)
            {
                Ref<DescriptorSet> set = DescriptorSet::Create(
                    m_Context, {
                                   .Name = fmt::format("SceneRenderer SSR Blur Set {}", level),
                                   .Layout = bloomDownUpLayout,
                               });
                set->Write(0, m_ReflectionMips[level - 1]);
                set->Write(1, m_ReflectionSampler);
                set->Write(2, m_ReflectionMips[level]);
                m_BlurSets.push_back(std::move(set));
            }
        }

        // Max-Z depth pyramid (mirrors the hi-Z reduction, opposite reduction): a full mip chain
        // the trace marches through to skip empty space. Reduced from this frame's depth before the
        // trace; distinct from the occlusion-culling min-Z pyramid.
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
    }

    void SsrChain::Declare(RenderGraph& graph, const ResourceId sceneId,
                           const MipChainId reflectionChainId, const MipChainId hiZChainId,
                           const ResourceId normalId, const ResourceId ormId,
                           const ResourceId depthId, const ResourceId hdrId,
                           const TextureHandle depthHandle, const TextureHandle normalHandle,
                           const TextureHandle ormHandle, const TextureHandle albedoHandle,
                           const SamplerHandle samplerHandle)
    {
        const u32 mipCount = static_cast<u32>(m_ReflectionMips.size());
        const u32 hizMips = static_cast<u32>(m_HiZMips.size());
        const uvec2 ssrExtent = RenderExtent();

        // Max-Z reduction: build this frame's closest-surface pyramid from the depth target
        // before the trace. One dispatch per mip; mip 0 ingests the full-res depth target into
        // the SSR-resolution pyramid (a downsample when SSR runs below full res, a 1:1 copy at
        // Full), deeper mips halve the prior. The per-mip graph surface derives the
        // read-after-write barriers; the trace then reads the whole chain.
        for (u32 level = 0; level < hizMips; level++)
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
                        .FirstSet = 1, // set 0 is reserved for the bindless registry
                        .PipelineBindPoint = PipelineBindPoint::Compute,
                    });
                    cmd.PushConstants(push);
                    cmd.Dispatch((push.DestExtent.x + 7) / 8, (push.DestExtent.y + 7) / 8, 1);
                });
        }

        // Trace: reflect the view vector about the g-buffer normal, march the depth buffer, and
        // write the reflected scene radiance + a confidence mask into the reflection chain's mip 0.
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
                        .MaxDistance = view.SsrMaxDistance,
                        .Thickness = view.SsrThickness,
                        .MaxRoughness = view.SsrMaxRoughness,
                        .MaxSteps = 256,
                    });
                    cmd.DrawFullscreenTriangle();
                });
        }

        // Blur down-sweep: build the roughness mip chain (mip k averages mip k-1). The per-mip
        // graph surface derives the read-after-write barrier between writing mip k and reading it.
        const uvec2 allocExtent = ssrExtent;
        for (u32 level = 1; level < mipCount; level++)
        {
            const u32 dstW = std::max(allocExtent.x >> level, 1u);
            const u32 dstH = std::max(allocExtent.y >> level, 1u);
            const Ref<ComputePipeline> pipeline = m_BlurPipeline;
            const Ref<DescriptorSet> set = m_BlurSets[level - 1];

            graph.AddComputePass(fmt::format("SSR Blur Mip {}", level))
                .Sample(reflectionChainId.Level(level - 1))
                .StorageWrite(reflectionChainId.Level(level))
                .Execute(
                    [pipeline, set, dstW, dstH](PassContext& inner)
                    {
                        CommandBuffer& cmd = inner.Cmd();
                        cmd.BindPipeline(pipeline);
                        cmd.BindDescriptorSets(DescriptorSetBindInfo{
                            .Sets = {set},
                            .FirstSet = 1, // set 0 is reserved for the bindless registry
                            .PipelineBindPoint = PipelineBindPoint::Compute,
                        });
                        cmd.PushConstants(SsrBlurPush{.DestExtent = {dstW, dstH}});
                        cmd.Dispatch((dstW + 7) / 8, (dstH + 7) / 8, 1);
                    });
        }

        // Composite: fold the (roughness-LOD) reflection into the scene color and write the HDR
        // target the bloom/tonemap tail reads.
        {
            const TextureHandle sceneHandle = m_SceneHandle;
            const TextureHandle reflectionHandle = m_ReflectionSampleHandle;
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
                .Sample(normalId)
                .Sample(ormId)
                .Sample(depthId);
            // Every reflection mip the composite's LOD sampling may read must be ShaderReadOnly.
            for (u32 level = 0; level < mipCount; level++)
            {
                builder.Sample(reflectionChainId.Level(level));
            }

            builder.Execute(
                [this, sceneHandle, reflectionHandle, albedoHandle, normalHandle, ormHandle,
                 depthHandle, samplerHandle, reflSamplerHandle, mipCount](PassContext& inner)
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
                        .AlbedoTexture = albedoHandle.Index,
                        .NormalTexture = normalHandle.Index,
                        .OrmTexture = ormHandle.Index,
                        .DepthTexture = depthHandle.Index,
                        .Sampler = samplerHandle.Index,
                        .ReflectionSampler = reflSamplerHandle.Index,
                        .ViewConstantsIndex = registry.GetCurrentViewConstantsIndex(),
                        .Intensity = view.SsrIntensity,
                        .MipCount = mipCount,
                    });
                    cmd.DrawFullscreenTriangle();
                });
        }
    }
}
