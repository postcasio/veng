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
    class Buffer;
    class Image;
    class ImageView;
    class DescriptorSet;
    class DescriptorSetLayout;
    class ComputePipeline;
    class PipelineLayout;
    struct SceneRendererSettings;
    struct SsrTraceTargets;

    /// @brief Owns the stochastic screen-space-reflection chain: the depth pyramid, tile classification, the work lists and the intersection.
    ///
    /// Per frame the chain reduces the depth target to a nearest-depth pyramid (ssr_hiz_reduce.comp
    /// and its one-dispatch tail, level 0 a copy of the depth at the render allocation, seven
    /// levels at most). The classifier (ssr_classify.comp) decides which pixels trace a
    /// reflection ray — mirrors, each 2x2 quad's base ray at the SsrDensity, and pixels the
    /// denoiser's variance flags — and compacts them into the ray list, lists the 8x8 tiles holding
    /// a glossy pixel, extracts each pixel's GGX α and clears the radiance and ray-length targets.
    /// The argument pass (ssr_prepare_args.comp) writes the indirect dispatch records of the passes
    /// consuming the lists and moves each total into its count slot (Veng/ssr_ray_list.slang). The
    /// intersection (ssr_intersect.comp), dispatched through the first record, traces each listed
    /// ray through the pyramid and writes the lit scene it meets, premultiplied by the hit's
    /// confidence, and the ray's world-space length, to the ray's pixel and the quad neighbours it
    /// covers. Every target is at the render allocation, renderer-owned, and imported into the
    /// graph by Import.
    ///
    /// The chain also holds the blue-noise tile its rays sample their GGX lobe from
    /// (Veng/ssr_sample.slang), loaded from the core pack the first time the chain is allocated.
    ///
    /// Storage targets bind on set 3 through each pass's own descriptor set; the g-buffer, the
    /// pyramid, the lit scene, the view block and the variance are read through bindless set 0. The
    /// pyramid's reduce and tail pipelines build on the renderer's hi-Z reduce and tail layouts,
    /// which must exist when the chain is created.
    class SsrStochasticChain
    {
    public:
        /// @brief Creates the chain's layouts and pipelines (its targets are built by Recreate).
        /// @param context         The render context the resources are created on.
        /// @param assets          Asset manager the chain's shaders and blue-noise tile load from.
        /// @param hiZReduceLayout The hi-Z reduce pipeline layout the pyramid's reduce builds on.
        /// @param hiZTailLayout   The hi-Z coarse-tail pipeline layout the pyramid's tail builds on.
        /// @return A new SsrStochasticChain.
        static Unique<SsrStochasticChain> Create(Context& context, AssetManager& assets,
                                                 const Ref<PipelineLayout>& hiZReduceLayout,
                                                 const Ref<PipelineLayout>& hiZTailLayout);

        /// @brief Releases the chain's bindless slots; the images and buffers retire through the frame bin.
        ~SsrStochasticChain();

        SsrStochasticChain(const SsrStochasticChain&) = delete;
        SsrStochasticChain& operator=(const SsrStochasticChain&) = delete;

        /// @brief Allocates the chain's targets at @p extent while it runs, releasing them otherwise.
        ///
        /// The chain runs when SSR does (Settings.SSR in the Final view, or the Reflections debug
        /// arm) under SsrTechnique::Stochastic. The counters start at zero, so the first classify
        /// appends to empty lists. Called from the renderer's Create and every Resize/Configure.
        /// @param settings           The active renderer settings (the gate and the ray density).
        /// @param extent             The render allocation the chain covers.
        /// @param depthView          The live depth target the pyramid's level 0 copies.
        /// @param hiZReduceSetLayout The hi-Z reduce set layout the pyramid's reduce sets allocate from.
        /// @param hiZTailSetLayout   The hi-Z coarse-tail set layout the pyramid's tail set allocates from.
        void Recreate(const SceneRendererSettings& settings, uvec2 extent,
                      const Ref<ImageView>& depthView,
                      const Ref<DescriptorSetLayout>& hiZReduceSetLayout,
                      const Ref<DescriptorSetLayout>& hiZTailSetLayout);

        /// @brief Whether Recreate allocated the chain's targets.
        [[nodiscard]] bool IsAllocated() const { return m_RadianceImage != nullptr; }

        /// @brief Imports the chain's lists, counters, arguments and targets into the graph.
        ///
        /// Called while the graph's imports are declared, ahead of the passes, so a pass reading the
        /// radiance target can name it (GetRadianceId). The imports bind through AppendBindings.
        /// @param graph The renderer's internal graph being rebuilt.
        /// @pre IsAllocated().
        void Import(RenderGraph& graph);

        /// @brief Declares the pyramid, classify, argument and intersection passes into the graph.
        ///
        /// Declared after the last pass writing the lit scene, on the imports Import declared: the
        /// intersection reads the scene as it stands then.
        /// @param graph        The renderer's internal graph being rebuilt.
        /// @param depthId      The depth import (sampled).
        /// @param normalId     The G1 world-normal import (sampled).
        /// @param ormId        The G2 packed occlusion/roughness/metallic import (sampled).
        /// @param sceneId      The lit scene the rays reflect (sampled).
        /// @param depthHandle  Bindless slot for the depth target.
        /// @param normalHandle Bindless slot for the G1 normal.
        /// @param ormHandle    Bindless slot for the G2 ORM.
        /// @param sceneHandle  Bindless slot for the lit scene.
        /// @pre Import ran on @p graph.
        void Declare(RenderGraph& graph, ResourceId depthId, ResourceId normalId, ResourceId ormId,
                     ResourceId sceneId, TextureHandle depthHandle, TextureHandle normalHandle,
                     TextureHandle ormHandle, TextureHandle sceneHandle);

        /// @brief Appends the chain's import bindings for this Execute (nothing when unallocated).
        /// @param bindings The renderer's import bindings.
        void AppendBindings(std::vector<RenderGraph::ImportBinding>& bindings) const;

        /// @brief The radiance target's import, declared by the last Import.
        [[nodiscard]] ResourceId GetRadianceId() const { return m_RadianceId; }

        /// @brief Bindless slot for the radiance target (the Reflections debug arm blits it).
        [[nodiscard]] TextureHandle GetRadianceHandle() const { return m_RadianceHandle; }

        /// @brief Bindless slot for the extracted-α target.
        [[nodiscard]] TextureHandle GetAlphaHandle() const { return m_AlphaHandle; }

        /// @brief Bindless slot for the blue-noise tile (RG8, SsrBlueNoiseSize square), valid once allocated.
        [[nodiscard]] TextureHandle GetBlueNoiseHandle() const { return m_BlueNoiseHandle; }

        /// @brief The chain's lists, counters, arguments and targets, for tests and tooling.
        /// @return The current targets; every member is null while the chain is unallocated.
        [[nodiscard]] SsrTraceTargets GetTargets() const;

    private:
        SsrStochasticChain(Context& context, AssetManager& assets,
                           const Ref<PipelineLayout>& hiZReduceLayout,
                           const Ref<PipelineLayout>& hiZTailLayout);

        /// @brief Loads and uploads the blue-noise tile from the core pack, once.
        void EnsureBlueNoise();

        /// @brief Allocates the depth pyramid, its views and its reduce and tail sets.
        /// @param depthView          The depth target level 0 copies.
        /// @param hiZReduceSetLayout The layout the per-level reduce sets allocate from.
        /// @param hiZTailSetLayout   The layout the tail set allocates from.
        void CreatePyramid(const Ref<ImageView>& depthView,
                           const Ref<DescriptorSetLayout>& hiZReduceSetLayout,
                           const Ref<DescriptorSetLayout>& hiZTailSetLayout);

        /// @brief Declares the pyramid's per-level reductions and its coarse tail.
        /// @param graph   The graph being rebuilt.
        /// @param depthId The depth import level 0 reads.
        void DeclarePyramid(RenderGraph& graph, ResourceId depthId);

        Context& m_Context;
        AssetManager& m_Assets;

        /// @brief The render allocation the targets cover (set by Recreate).
        uvec2 m_Extent{1};
        /// @brief Rays per quad the classifier keeps: 1, 2 or 4 (set by Recreate).
        u32 m_SamplesPerQuad = 1;

        /// @brief Classify pipeline (64x1 groups, one per 8x8 tile).
        Ref<ComputePipeline> m_ClassifyPipeline;
        /// @brief The classifier's set 3: the ray list, tile list and counters, and the radiance and α targets.
        Ref<DescriptorSetLayout> m_ClassifySetLayout;
        /// @brief Layout for m_ClassifyPipeline (m_ClassifySetLayout + the classify push block).
        Ref<PipelineLayout> m_ClassifyLayout;
        /// @brief Argument pipeline (one invocation).
        Ref<ComputePipeline> m_PreparePipeline;
        /// @brief Intersection pipeline (64 rays a group, dispatched indirectly).
        Ref<ComputePipeline> m_IntersectPipeline;
        /// @brief The intersection's set 3: the ray list, the counters, and the radiance and ray-length targets.
        Ref<DescriptorSetLayout> m_IntersectSetLayout;
        /// @brief Layout for m_IntersectPipeline.
        Ref<PipelineLayout> m_IntersectLayout;
        /// @brief The pyramid's per-level reduce pipeline, on the hi-Z reduce layout.
        Ref<ComputePipeline> m_PyramidReducePipeline;
        /// @brief The pyramid's coarse-tail pipeline, on the hi-Z tail layout.
        Ref<ComputePipeline> m_PyramidTailPipeline;
        /// @brief The argument pass's set 3: the counters and the arguments.
        Ref<DescriptorSetLayout> m_PrepareSetLayout;
        /// @brief Layout for m_PreparePipeline.
        Ref<PipelineLayout> m_PrepareLayout;

        /// @brief The classifier's set, written by Recreate.
        Ref<DescriptorSet> m_ClassifySet;
        /// @brief The argument pass's set, written by Recreate.
        Ref<DescriptorSet> m_PrepareSet;
        /// @brief The intersection's set, written by Recreate.
        Ref<DescriptorSet> m_IntersectSet;

        /// @brief One packed ray per pixel at most: the pixel and its quad copy flags.
        Ref<Buffer> m_RayList;
        /// @brief One packed tile origin per 8x8 tile at most.
        Ref<Buffer> m_TileList;
        /// @brief The four counters: the ray and tile append slots and their count slots.
        Ref<Buffer> m_Counters;
        /// @brief Two dispatch records: the intersect's, then the denoiser's.
        Ref<Buffer> m_Arguments;

        /// @brief The reflected radiance, premultiplied by its confidence (RGBA16F), cleared by the classifier.
        Ref<Image> m_RadianceImage;
        /// @brief View over m_RadianceImage.
        Ref<ImageView> m_RadianceView;
        /// @brief Bindless slot for m_RadianceView.
        TextureHandle m_RadianceHandle;
        /// @brief Each pixel's GGX α (R8), written by the classifier.
        Ref<Image> m_AlphaImage;
        /// @brief View over m_AlphaImage.
        Ref<ImageView> m_AlphaView;
        /// @brief Bindless slot for m_AlphaView.
        TextureHandle m_AlphaHandle;
        /// @brief Each ray's world-space length to its hit (R16F), zero for a miss, cleared by the classifier.
        Ref<Image> m_RayLengthImage;
        /// @brief View over m_RayLengthImage.
        Ref<ImageView> m_RayLengthView;

        /// @brief The nearest-depth pyramid (R32F), level 0 the depth at the render allocation.
        Ref<Image> m_PyramidImage;
        /// @brief One single-level view per pyramid level: the reduce's sources and destinations.
        std::vector<Ref<ImageView>> m_PyramidMips;
        /// @brief A view over every pyramid level, which the intersection reads.
        Ref<ImageView> m_PyramidSampleView;
        /// @brief Bindless slot for m_PyramidSampleView.
        TextureHandle m_PyramidHandle;
        /// @brief Per-level reduce sets: level k's source and destination.
        std::vector<Ref<DescriptorSet>> m_PyramidReduceSets;
        /// @brief The coarse tail's set, or null when the pyramid has no tail.
        Ref<DescriptorSet> m_PyramidTailSet;
        /// @brief The tail's first level (MipTailFirstLevel), meaningful while m_PyramidTailSet is set.
        u32 m_PyramidTailFirst = 0;

        /// @brief The blue-noise tile the rays draw their lobe samples from.
        Ref<Image> m_BlueNoiseImage;
        /// @brief View over m_BlueNoiseImage.
        Ref<ImageView> m_BlueNoiseView;
        /// @brief Bindless slot for m_BlueNoiseView.
        TextureHandle m_BlueNoiseHandle;

        /// @brief The ray list's import, declared by Import.
        ResourceId m_RayListId;
        /// @brief The tile list's import.
        ResourceId m_TileListId;
        /// @brief The counters' import.
        ResourceId m_CountersId;
        /// @brief The arguments' import.
        ResourceId m_ArgumentsId;
        /// @brief The radiance target's import.
        ResourceId m_RadianceId;
        /// @brief The α target's import.
        ResourceId m_AlphaId;
        /// @brief The ray-length target's import.
        ResourceId m_RayLengthId;
        /// @brief The pyramid's per-level imports.
        MipChainId m_PyramidChainId;
    };
}
