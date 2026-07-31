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
    struct SceneView;

    /// @brief The depth-of-field composite push block, matching dof_composite.frag.
    ///
    /// Declared beside the chain because the chain builds the pipeline layout it is ranged on
    /// while DofCompositeScenePass writes it.
    struct DofCompositePush
    {
        /// @brief Bindless slot of the full-resolution lit scene color the layers cover.
        u32 HdrTexture;
        /// @brief Bindless slot of the near-field gathered + filled layer.
        u32 NearTexture;
        /// @brief Bindless slot of the far-field gathered + filled layer.
        u32 FarTexture;
        /// @brief Bindless slot of the chain's linear clamp sampler.
        u32 Sampler;
        /// @brief Full-resolution source validExtent/allocExtent.
        vec2 ScaleUV;
        /// @brief Full-resolution source (validExtent - 0.5)/allocExtent.
        vec2 MaxUV;
        /// @brief Half-resolution layer validExtent/allocExtent.
        vec2 HalfScaleUV;
        /// @brief Half-resolution layer (validExtent - 0.5)/allocExtent.
        vec2 HalfMaxUV;
    };

    /// @brief The half-resolution extent a full render extent maps to (rounded up, floored at 1).
    /// @param extent  The full extent.
    /// @return Half of it per axis.
    [[nodiscard]] inline uvec2 DofHalfExtent(const uvec2 extent)
    {
        return glm::max(uvec2(1), (extent + 1u) / 2u);
    }

    /// @brief Owns the depth-of-field battery — the half-resolution near/far layers, the tile
    ///        records, the ring-gather and fill targets, and the five pipelines behind them.
    ///
    /// The defocus vertical the renderer splices into the HDR tail ahead of bloom. Recreate
    /// allocates the chain when depth of field runs (the toggle or the CoC debug arm) and releases
    /// it otherwise; Declare contributes circle-of-confusion prefilter -> tile reduction ->
    /// per-layer ring gather -> per-layer fill into the graph, and the renderer's
    /// DofCompositeScenePass blends the layers over the chain's lit scene-color intermediate into
    /// the HDR target — so the bloom/tonemap tail reads the same id it always did.
    ///
    /// Half resolution is half the *allocation* extent: every dispatch and every sample clamps to
    /// this frame's valid sub-rect through the render-scale mapping the chain derives per frame,
    /// the dynamic-resolution convention the rest of the renderer follows.
    ///
    /// Two of the chain's descriptor allocations borrow the bloom down/up set layout (a sampled
    /// source, a sampler, one storage destination): the tile reduction and the two fill passes.
    /// It is received by reference at construction, so the lending layout must already exist when
    /// DofChain is created.
    class DofChain
    {
    public:
        /// @brief Creates the depth-of-field pipelines and layouts (the targets are built by Recreate).
        /// @param context           The render context the resources are created on.
        /// @param assets            Asset manager used to load the depth-of-field shaders.
        /// @param bloomDownUpLayout The bloom down/up set layout the tile and fill stages reserve.
        /// @param compositeFormat   Color format of the HDR intermediate the composite writes.
        /// @return A new DofChain.
        static Unique<DofChain> Create(Context& context, AssetManager& assets,
                                       const Ref<DescriptorSetLayout>& bloomDownUpLayout,
                                       Format compositeFormat);

        /// @brief Releases the chain's bindless slots; the images retire through the frame bin.
        ~DofChain();

        DofChain(const DofChain&) = delete;
        DofChain& operator=(const DofChain&) = delete;

        /// @brief Recreates the half-resolution layers, tile grid, gather/fill targets, and result.
        ///
        /// Allocates them when depth of field runs (Settings.DepthOfField in the Final view, or the
        /// CoC debug arm); otherwise releases any previously-created ones. Called from the
        /// renderer's Create and every Resize/Configure.
        /// @param settings          The active renderer settings (the DepthOfField/Mode gate).
        /// @param extent            The full render allocation extent; the chain is half of it.
        /// @param hdrView           The HDR target the prefilter samples in the debug arm (the
        ///                          chain's own scene intermediate is the source when composited).
        /// @param depthView         The live depth target the prefilter reconstructs depth from.
        /// @param bloomDownUpLayout The bloom down/up set layout the tile and fill sets allocate from.
        void Recreate(const SceneRendererSettings& settings, uvec2 extent,
                      const Ref<ImageView>& hdrView, const Ref<ImageView>& depthView,
                      const Ref<DescriptorSetLayout>& bloomDownUpLayout);

        /// @brief Declares the prefilter, tile reduction, ring gather, and fill passes into the graph.
        /// @param graph      The renderer's internal graph being rebuilt.
        /// @param sourceId   The lit scene-color import the prefilter samples.
        /// @param depthId    The depth import the prefilter samples.
        /// @param nearId     The near-layer import.
        /// @param farId      The far-layer import.
        /// @param cocId      The signed-radius/depth import the tile reduction reads.
        /// @param tileId     The tile-record import.
        /// @param nearBlurId The near-layer gather destination import.
        /// @param farBlurId  The far-layer gather destination import.
        /// @param nearFillId The near-layer fill destination import.
        /// @param farFillId  The far-layer fill destination import.
        /// @param stagesOnly When true only the prefilter and tile reduction are declared — the
        ///                   debug arm, which inspects the circle of confusion without touching
        ///                   the HDR tail.
        void Declare(RenderGraph& graph, ResourceId sourceId, ResourceId depthId, ResourceId nearId,
                     ResourceId farId, ResourceId cocId, ResourceId tileId, ResourceId nearBlurId,
                     ResourceId farBlurId, ResourceId nearFillId, ResourceId farFillId,
                     bool stagesOnly);

        /// @brief The near-layer prefilter target view.
        [[nodiscard]] const Ref<ImageView>& GetNearView() const { return m_NearView; }
        /// @brief The far-layer prefilter target view.
        [[nodiscard]] const Ref<ImageView>& GetFarView() const { return m_FarView; }
        /// @brief The signed-radius/view-depth target view the tile reduction reads.
        [[nodiscard]] const Ref<ImageView>& GetCocView() const { return m_CocView; }
        /// @brief The tile-record target view.
        [[nodiscard]] const Ref<ImageView>& GetTileView() const { return m_TileView; }
        /// @brief The near-layer ring-gather destination view.
        [[nodiscard]] const Ref<ImageView>& GetNearBlurView() const { return m_NearBlurView; }
        /// @brief The far-layer ring-gather destination view.
        [[nodiscard]] const Ref<ImageView>& GetFarBlurView() const { return m_FarBlurView; }
        /// @brief The near-layer fill destination view (the composite's near source).
        [[nodiscard]] const Ref<ImageView>& GetNearFillView() const { return m_NearFillView; }
        /// @brief The far-layer fill destination view (the composite's far source).
        [[nodiscard]] const Ref<ImageView>& GetFarFillView() const { return m_FarFillView; }
        /// @brief The lit scene-color intermediate the composite reads and defocuses.
        ///
        /// Present only when the chain is fully wired: lighting (and any SSR composite or point
        /// field) writes here instead of the HDR target, and the composite writes the HDR target,
        /// so the bloom/tonemap tail is unchanged.
        [[nodiscard]] const Ref<ImageView>& GetSceneView() const { return m_SceneView; }

        /// @brief Bindless slot for the signed-radius/view-depth target (the CoC debug arm blits it).
        [[nodiscard]] TextureHandle GetCocHandle() const { return m_CocHandle; }
        /// @brief Bindless slot for the near-layer fill target (the composite samples it).
        [[nodiscard]] TextureHandle GetNearFillHandle() const { return m_NearFillHandle; }
        /// @brief Bindless slot for the far-layer fill target (the composite samples it).
        [[nodiscard]] TextureHandle GetFarFillHandle() const { return m_FarFillHandle; }
        /// @brief Bindless slot for the lit scene-color intermediate (the composite samples it).
        [[nodiscard]] TextureHandle GetSceneHandle() const { return m_SceneHandle; }
        /// @brief Bindless slot for the chain's linear clamp sampler.
        [[nodiscard]] SamplerHandle GetSamplerHandle() const { return m_SamplerHandle; }

        /// @brief The composite pipeline the renderer's DofCompositeScenePass records with.
        [[nodiscard]] const Ref<GraphicsPipeline>& GetCompositePipeline() const
        {
            return m_CompositePipeline;
        }

        /// @brief The half-resolution allocation extent the chain's targets live in.
        [[nodiscard]] uvec2 GetHalfExtent() const;

    private:
        DofChain(Context& context, AssetManager& assets,
                 const Ref<DescriptorSetLayout>& bloomDownUpLayout, Format compositeFormat);

        /// @brief Releases every bindless slot the chain holds and clears the handles.
        void ReleaseHandles();

        Context& m_Context;

        /// @brief The full render allocation extent the chain is half of (set by Recreate).
        uvec2 m_Extent{1};

        /// @brief Circle-of-confusion + prefilter compute pipeline.
        Ref<ComputePipeline> m_PrefilterPipeline;
        /// @brief Layout for m_PrefilterPipeline (its own set plus the prefilter push block).
        Ref<PipelineLayout> m_PrefilterLayout;
        /// @brief Set layout for the prefilter's sources and its three storage destinations.
        Ref<DescriptorSetLayout> m_PrefilterSetLayout;

        /// @brief Tile reduction + dilation compute pipeline (over the bloom down/up set layout).
        Ref<ComputePipeline> m_TilePipeline;
        /// @brief Layout for m_TilePipeline (the borrowed bloom set + the tile push block).
        Ref<PipelineLayout> m_TileLayout;

        /// @brief Ring-gather compute pipeline, dispatched once per layer.
        Ref<ComputePipeline> m_GatherPipeline;
        /// @brief Layout for m_GatherPipeline (its own set plus the gather push block).
        Ref<PipelineLayout> m_GatherLayout;
        /// @brief Set layout for a gather dispatch (the layer, the tile records, a storage dest).
        Ref<DescriptorSetLayout> m_GatherSetLayout;

        /// @brief Fill compute pipeline, dispatched once per layer (over the bloom down/up layout).
        Ref<ComputePipeline> m_FillPipeline;
        /// @brief Layout for m_FillPipeline (the borrowed bloom set + the fill push block).
        Ref<PipelineLayout> m_FillLayout;

        /// @brief Fullscreen composite pipeline writing the HDR intermediate.
        Ref<GraphicsPipeline> m_CompositePipeline;
        /// @brief Layout for m_CompositePipeline (the composite push block).
        Ref<PipelineLayout> m_CompositeLayout;

        /// @brief Linear clamp-to-edge sampler every stage reads its sources through, shared out of
        /// the bindless registry with every other consumer of the same settings.
        Ref<Sampler> m_Sampler;
        /// @brief Bindless slot for m_Sampler (the composite's bindless sampler index).
        SamplerHandle m_SamplerHandle;

        /// @brief Near-field layer: scene color with the near radius in alpha.
        Ref<Image> m_NearImage;
        /// @brief View over m_NearImage.
        Ref<ImageView> m_NearView;
        /// @brief Far-field layer: scene color with the far radius in alpha.
        Ref<Image> m_FarImage;
        /// @brief View over m_FarImage.
        Ref<ImageView> m_FarView;
        /// @brief Signed radius and view-space depth, the tile reduction's only source.
        Ref<Image> m_CocImage;
        /// @brief View over m_CocImage.
        Ref<ImageView> m_CocView;
        /// @brief Bindless slot for m_CocView.
        TextureHandle m_CocHandle;

        /// @brief Per-tile records bounding each gather's kernel radius.
        Ref<Image> m_TileImage;
        /// @brief View over m_TileImage.
        Ref<ImageView> m_TileView;

        /// @brief Near-layer ring-gather destination.
        Ref<Image> m_NearBlurImage;
        /// @brief View over m_NearBlurImage.
        Ref<ImageView> m_NearBlurView;
        /// @brief Far-layer ring-gather destination.
        Ref<Image> m_FarBlurImage;
        /// @brief View over m_FarBlurImage.
        Ref<ImageView> m_FarBlurView;

        /// @brief Near-layer fill destination, the composite's near source.
        Ref<Image> m_NearFillImage;
        /// @brief View over m_NearFillImage.
        Ref<ImageView> m_NearFillView;
        /// @brief Bindless slot for m_NearFillView.
        TextureHandle m_NearFillHandle;
        /// @brief Far-layer fill destination, the composite's far source.
        Ref<Image> m_FarFillImage;
        /// @brief View over m_FarFillImage.
        Ref<ImageView> m_FarFillView;
        /// @brief Bindless slot for m_FarFillView.
        TextureHandle m_FarFillHandle;

        /// @brief The full-resolution lit scene-color intermediate the composite defocuses.
        Ref<Image> m_SceneImage;
        /// @brief View over m_SceneImage.
        Ref<ImageView> m_SceneView;
        /// @brief Bindless slot for m_SceneView.
        TextureHandle m_SceneHandle;

        /// @brief Prefilter set: the HDR and depth sources, the sampler, the three destinations.
        Ref<DescriptorSet> m_PrefilterSet;
        /// @brief Tile set: the radius/depth source, the sampler, the tile destination.
        Ref<DescriptorSet> m_TileSet;
        /// @brief Near-layer gather set: the near layer, the tile records, the gather destination.
        Ref<DescriptorSet> m_NearGatherSet;
        /// @brief Far-layer gather set: the far layer, the tile records, the gather destination.
        Ref<DescriptorSet> m_FarGatherSet;
        /// @brief Near-layer fill set: the gathered near layer and the fill destination.
        Ref<DescriptorSet> m_NearFillSet;
        /// @brief Far-layer fill set: the gathered far layer and the fill destination.
        Ref<DescriptorSet> m_FarFillSet;
    };
}
