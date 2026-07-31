#pragma once

#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Veng.h>

#include <vector>

namespace Veng
{
    class AssetManager;
}

namespace Veng::Renderer
{
    class Context;
    class Image;
    class ImageView;
    class Sampler;
    class DescriptorSet;
    class DescriptorSetLayout;
    class GraphicsPipeline;
    class ComputePipeline;
    class PipelineLayout;
    struct SceneRendererSettings;

    /// @brief Owns the screen-space-reflection chain — scene-color intermediate, mip chain, min-Z pyramid, and sweep.
    ///
    /// The reflection vertical the renderer wires between lighting and the bloom/tonemap tail: the lit
    /// scene-color intermediate the trace samples (lighting/TAA writes it, the composite reflects into it
    /// and writes the HDR target), the reflection mip chain (mip 0 the trace writes, coarser mips the
    /// blur produces), the min-Z depth pyramid the hi-Z trace marches, and the four pipelines — min-Z
    /// reduction, trace, blur, composite. Recreate allocates the whole chain at the SsrResolution scale
    /// when SSR runs (the toggle or the Reflections debug arm), releasing it otherwise; Declare
    /// contributes min-Z reduction → trace → blur → composite into the graph.
    ///
    /// Two of the chain's descriptor allocations borrow layouts owned elsewhere: the blur sets and the
    /// blur pipeline layout use the bloom down/up set layout (from BloomPyramid), and the min-Z reduce
    /// sets and pipeline build on the renderer's hi-Z reduce layout. Both are received by reference at
    /// construction; the blur pipeline (bloom set layout) and the min-Z reduce pipeline (hi-Z reduce
    /// layout) are built in the constructor, so the lending layouts must already exist when SsrChain is
    /// created.
    class SsrChain
    {
    public:
        /// @brief Creates the SSR pipelines and layouts (the chain resources are built by Recreate).
        /// @param context           The render context the resources are created on.
        /// @param assets            Asset manager used to load the SSR shaders.
        /// @param hiZReduceLayout   The hi-Z reduce pipeline layout (the min-Z reduce pipeline builds on it).
        /// @param bloomDownUpLayout The bloom down/up set layout (the blur pipeline layout reserves it).
        /// @return A new SsrChain.
        static Unique<SsrChain> Create(Context& context, AssetManager& assets,
                                       const Ref<PipelineLayout>& hiZReduceLayout,
                                       const Ref<DescriptorSetLayout>& bloomDownUpLayout);

        /// @brief Releases the four SSR bindless slots; the images retire through the frame bin.
        ~SsrChain();

        SsrChain(const SsrChain&) = delete;
        SsrChain& operator=(const SsrChain&) = delete;

        /// @brief Recreates the scene-color intermediate, reflection mip chain, and min-Z pyramid.
        ///
        /// Allocates the targets and per-level descriptor sets when SSR runs (Settings.SSR or the
        /// Reflections debug arm); otherwise releases any previously-created ones. The reflection blur
        /// chain mirrors the bloom pyramid, the min-Z pyramid the hi-Z reduction. Called from the
        /// renderer's Create and every Resize/Configure.
        /// @param settings          The active renderer settings (SSR/Mode gate allocation, SsrResolutionScale sizes the chain).
        /// @param extent            The full render extent (the scene-color intermediate is full-res).
        /// @param depthView         The live depth target the min-Z reduction's mip-0 source binds.
        /// @param hiZReduceSetLayout The hi-Z reduce set layout the min-Z reduce sets allocate from.
        /// @param bloomDownUpLayout The bloom down/up set layout the blur sets allocate from.
        void Recreate(const SceneRendererSettings& settings, uvec2 extent,
                      const Ref<ImageView>& depthView,
                      const Ref<DescriptorSetLayout>& hiZReduceSetLayout,
                      const Ref<DescriptorSetLayout>& bloomDownUpLayout);

        /// @brief Declares the min-Z reduction, trace, blur, and composite passes into the graph.
        ///
        /// Reduces this frame's depth to a min-Z pyramid, traces reflections against it into the
        /// reflection chain's mip 0, blurs the chain for rough reflections, then composites the
        /// reflection into the HDR target the bloom/tonemap tail reads. Declared between the lighting
        /// pass and the bloom sweep.
        /// @param graph              The renderer's internal graph being rebuilt.
        /// @param sceneId            The lit scene-color intermediate import (trace + composite sample).
        /// @param reflectionChainId  The per-mip reflection pyramid import (trace + blur).
        /// @param hiZChainId         The per-mip min-Z pyramid import (reduction + trace).
        /// @param normalId           The G1 world-normal import (declared sampled for barrier order).
        /// @param ormId              The G2 packed occlusion/roughness/metallic import.
        /// @param depthId            The depth import.
        /// @param hdrId              The HDR target the composite writes.
        /// @param depthHandle        Bindless slot for the depth target.
        /// @param normalHandle       Bindless slot for the G1 normal.
        /// @param ormHandle          Bindless slot for the G2 ORM.
        /// @param albedoHandle       Bindless slot for the G0 albedo (the composite's metallic F0 tint).
        /// @param samplerHandle      Shared linear sampler bindless slot.
        void Declare(RenderGraph& graph, ResourceId sceneId, MipChainId reflectionChainId,
                     MipChainId hiZChainId, ResourceId normalId, ResourceId ormId,
                     ResourceId depthId, ResourceId hdrId, TextureHandle depthHandle,
                     TextureHandle normalHandle, TextureHandle ormHandle,
                     TextureHandle albedoHandle, SamplerHandle samplerHandle);

        /// @brief The lit scene-color intermediate view (bound to its import when SSR is active).
        [[nodiscard]] const Ref<ImageView>& GetSceneView() const { return m_SceneView; }

        /// @brief Bindless slot for the scene-color intermediate (the lighting/TAA write target when SSR is on).
        [[nodiscard]] TextureHandle GetSceneHandle() const { return m_SceneHandle; }

        /// @brief Bindless slot for the reflection sample view (the Reflections debug arm blits it).
        [[nodiscard]] TextureHandle GetReflectionSampleHandle() const
        {
            return m_ReflectionSampleHandle;
        }

        /// @brief The per-level reflection storage views, bound to their per-mip import slots each Execute.
        [[nodiscard]] const std::vector<Ref<ImageView>>& GetReflectionMipViews() const
        {
            return m_ReflectionMips;
        }

        /// @brief Number of reflection mip levels (the import slot count and the chain binding range).
        [[nodiscard]] u32 GetReflectionMipCount() const
        {
            return static_cast<u32>(m_ReflectionMips.size());
        }

        /// @brief The per-level min-Z storage views, bound to their per-mip import slots each Execute.
        [[nodiscard]] const std::vector<Ref<ImageView>>& GetHiZMipViews() const
        {
            return m_HiZMips;
        }

        /// @brief Number of min-Z mip levels (the import slot count and the chain binding range).
        [[nodiscard]] u32 GetHiZMipCount() const { return static_cast<u32>(m_HiZMips.size()); }

    private:
        SsrChain(Context& context, AssetManager& assets, const Ref<PipelineLayout>& hiZReduceLayout,
                 const Ref<DescriptorSetLayout>& bloomDownUpLayout);

        /// @brief The pixel extent the trace/min-Z/blur chain runs at (SsrResolutionScale folded onto m_Extent).
        [[nodiscard]] uvec2 RenderExtent() const;

        Context& m_Context;

        /// @brief The full render extent the scene-color intermediate is sized to (set by Recreate).
        uvec2 m_Extent{1};
        /// @brief The SsrResolutionScale the trace/min-Z/blur chain runs at (set by Recreate).
        u8 m_ResolutionScale = 0;

        /// @brief SSR trace pipeline (fullscreen, writes the reflection chain's mip 0).
        Ref<GraphicsPipeline> m_TracePipeline;
        /// @brief Layout for m_TracePipeline (the trace push block).
        Ref<PipelineLayout> m_TraceLayout;
        /// @brief SSR composite pipeline (fullscreen, writes the HDR target).
        Ref<GraphicsPipeline> m_CompositePipeline;
        /// @brief Layout for m_CompositePipeline (the composite push block).
        Ref<PipelineLayout> m_CompositeLayout;
        /// @brief SSR reflection blur-downsample compute pipeline (over the bloom down/up set layout).
        Ref<ComputePipeline> m_BlurPipeline;
        /// @brief Layout for m_BlurPipeline (the borrowed bloom set + the blur push block).
        Ref<PipelineLayout> m_BlurLayout;
        /// @brief SSR min-Z reduction compute pipeline (over the borrowed hi-Z reduce layout).
        Ref<ComputePipeline> m_HiZReducePipeline;

        /// @brief Lit scene-color intermediate the trace samples and the composite adds onto.
        Ref<Image> m_SceneImage;
        /// @brief View over m_SceneImage.
        Ref<ImageView> m_SceneView;
        /// @brief Bindless slot for m_SceneView.
        TextureHandle m_SceneHandle;

        /// @brief Reflection mip chain: mip 0 the trace writes, coarser mips the blur produces.
        Ref<Image> m_ReflectionImage;
        /// @brief One single-mip view per reflection level (mip 0 a color target, deeper mips storage dests).
        std::vector<Ref<ImageView>> m_ReflectionMips;
        /// @brief Whole-chain sampled view of the reflection pyramid; the composite samples it by LOD.
        Ref<ImageView> m_ReflectionSampleView;
        /// @brief Bindless slot for m_ReflectionSampleView.
        TextureHandle m_ReflectionSampleHandle;
        /// @brief Trilinear clamp-to-edge sampler over the reflection mip chain (roughness LOD),
        /// shared out of the bindless registry with every other consumer of the same settings.
        Ref<Sampler> m_ReflectionSampler;
        /// @brief Bindless slot for m_ReflectionSampler; the registry's, so it stays valid.
        SamplerHandle m_ReflectionSamplerHandle;
        /// @brief One blur-downsample set per produced level k (reads mip k-1, writes mip k).
        std::vector<Ref<DescriptorSet>> m_BlurSets;

        /// @brief Min-Z depth pyramid: the closest-surface mip chain the trace marches.
        Ref<Image> m_HiZImage;
        /// @brief One single-mip storage view per min-Z level (the reduction writes each).
        std::vector<Ref<ImageView>> m_HiZMips;
        /// @brief Whole-chain sampled view of the min-Z pyramid; the trace Loads levels from it.
        Ref<ImageView> m_HiZSampleView;
        /// @brief Bindless slot for m_HiZSampleView.
        TextureHandle m_HiZSampleHandle;
        /// @brief One reduction set per min-Z level (binds the source and destination mip views).
        std::vector<Ref<DescriptorSet>> m_HiZReduceSets;
    };
}
