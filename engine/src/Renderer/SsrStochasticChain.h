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

    /// @brief Owns the stochastic screen-space-reflection chain: tile classification, the work lists, and their dispatch arguments.
    ///
    /// Per frame the classifier (ssr_classify.comp) decides which pixels trace a reflection ray —
    /// mirrors, each 2x2 quad's base ray at the SsrDensity, and pixels the denoiser's variance
    /// flags — and compacts them into the ray list, lists the 8x8 tiles holding a glossy pixel,
    /// extracts each pixel's GGX α and clears the radiance target. The argument pass
    /// (ssr_prepare_args.comp) then writes the indirect dispatch records of the passes consuming the
    /// lists and moves each total into its count slot (Veng/ssr_ray_list.slang). Every target is at
    /// the render allocation, renderer-owned, and imported into the graph by Declare.
    ///
    /// The chain also holds the blue-noise tile its rays sample their GGX lobe from
    /// (Veng/ssr_sample.slang), loaded from the core pack the first time the chain is allocated.
    ///
    /// Storage targets bind on set 3 through each pass's own descriptor set; the g-buffer, the view
    /// block and the variance are read through bindless set 0.
    class SsrStochasticChain
    {
    public:
        /// @brief Creates the chain's layouts and pipelines (its targets are built by Recreate).
        /// @param context The render context the resources are created on.
        /// @param assets  Asset manager the chain's shaders and blue-noise tile load from.
        /// @return A new SsrStochasticChain.
        static Unique<SsrStochasticChain> Create(Context& context, AssetManager& assets);

        /// @brief Releases the chain's bindless slots; the images and buffers retire through the frame bin.
        ~SsrStochasticChain();

        SsrStochasticChain(const SsrStochasticChain&) = delete;
        SsrStochasticChain& operator=(const SsrStochasticChain&) = delete;

        /// @brief Allocates the chain's targets at @p extent while it runs, releasing them otherwise.
        ///
        /// The chain runs when SSR does (Settings.SSR in the Final view, or the Reflections debug
        /// arm) under SsrTechnique::Stochastic. The counters start at zero, so the first classify
        /// appends to empty lists. Called from the renderer's Create and every Resize/Configure.
        /// @param settings The active renderer settings (the gate and the ray density).
        /// @param extent   The render allocation the chain covers.
        void Recreate(const SceneRendererSettings& settings, uvec2 extent);

        /// @brief Whether Recreate allocated the chain's targets.
        [[nodiscard]] bool IsAllocated() const { return m_RadianceImage != nullptr; }

        /// @brief Imports the chain's lists, counters, arguments and targets into the graph.
        ///
        /// Called while the graph's imports are declared, ahead of the passes, so a pass reading the
        /// radiance target can name it (GetRadianceId). The imports bind through AppendBindings.
        /// @param graph The renderer's internal graph being rebuilt.
        /// @pre IsAllocated().
        void Import(RenderGraph& graph);

        /// @brief Declares the classify and argument passes into the graph.
        ///
        /// Declared after the last pass writing the g-buffer, on the imports Import declared.
        /// @param graph        The renderer's internal graph being rebuilt.
        /// @param depthId      The depth import (sampled).
        /// @param normalId     The G1 world-normal import (sampled).
        /// @param ormId        The G2 packed occlusion/roughness/metallic import (sampled).
        /// @param depthHandle  Bindless slot for the depth target.
        /// @param normalHandle Bindless slot for the G1 normal.
        /// @param ormHandle    Bindless slot for the G2 ORM.
        /// @pre Import ran on @p graph.
        void Declare(RenderGraph& graph, ResourceId depthId, ResourceId normalId, ResourceId ormId,
                     TextureHandle depthHandle, TextureHandle normalHandle,
                     TextureHandle ormHandle);

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
        SsrStochasticChain(Context& context, AssetManager& assets);

        /// @brief Loads and uploads the blue-noise tile from the core pack, once.
        void EnsureBlueNoise();

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
        /// @brief The argument pass's set 3: the counters and the arguments.
        Ref<DescriptorSetLayout> m_PrepareSetLayout;
        /// @brief Layout for m_PreparePipeline.
        Ref<PipelineLayout> m_PrepareLayout;

        /// @brief The classifier's set, written by Recreate.
        Ref<DescriptorSet> m_ClassifySet;
        /// @brief The argument pass's set, written by Recreate.
        Ref<DescriptorSet> m_PrepareSet;

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
    };
}
