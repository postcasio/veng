#pragma once

#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Veng.h>

#include <array>
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

    /// @brief Owns the screen-space-reflection chain — scene-color intermediate, mip chains, min-Z pyramid, and sweep.
    ///
    /// The reflection vertical the renderer wires between lighting and the bloom/tonemap tail: three
    /// mip chains the trace writes level 0 of and the blur low-passes down — the reflected radiance
    /// and the hit distance, both premultiplied by the trace's confidence, and the surface each
    /// texel's ray left from, premultiplied by its coverage — the min-Z depth pyramid the hi-Z trace
    /// marches, and the pipelines: min-Z reduction, trace, blur, composite, and the temporal pass.
    /// Recreate allocates the whole chain at the SsrResolution scale when SSR runs (the toggle or the
    /// Reflections debug arm), releasing it otherwise; Declare contributes min-Z reduction → trace →
    /// blur → composite into the graph.
    ///
    /// With no temporal resolve the composite folds the reflection into a lit scene-color
    /// intermediate the chain owns (lighting writes it) and writes the target the tail reads. Under
    /// one (ResolveSsrTemporal) the reflection is kept out of the resolve's history, which would
    /// reproject it by its surface's motion and lag it under every camera translation: the trace
    /// reads the resolve's lit target, the composite writes the reflection term and its reflected
    /// image's motion, and DeclareTemporal — declared after the resolve's history copy — accumulates
    /// the term into a ping-ponged history of the chain's own, reprojected by that motion, and adds it
    /// to the resolved scene.
    ///
    /// The mip chains stop where their short axis reaches two texels: the composite reconstructs a
    /// coarse level with a B-spline, so the widest reflected cone reads a smooth average of the
    /// screen rather than a blocky one.
    ///
    /// The min-Z reduce sets and pipeline, and its coarse-tail set and pipeline, build on the
    /// renderer's hi-Z reduce and tail layouts, received by reference; the min-Z reduce and tail
    /// pipelines are built in the constructor, so those layouts must already exist when SsrChain is
    /// created.
    ///
    /// The min-Z pyramid's coarse tail — the smallest levels that fit one workgroup's shared memory
    /// (MipTailFirstLevel), never level 0, which ingests the full-resolution depth — is reduced by one
    /// dispatch (ssr_hiz_reduce_tail.comp), bit-identical to the per-level chain.
    class SsrChain
    {
    public:
        /// @brief Creates the SSR pipelines and layouts (the chain resources are built by Recreate).
        /// @param context           The render context the resources are created on.
        /// @param assets            Asset manager used to load the SSR shaders.
        /// @param hiZReduceLayout   The hi-Z reduce pipeline layout (the min-Z reduce pipeline builds on it).
        /// @param hiZTailLayout     The hi-Z coarse-tail pipeline layout (the min-Z tail builds on it).
        /// @return A new SsrChain.
        static Unique<SsrChain> Create(Context& context, AssetManager& assets,
                                       const Ref<PipelineLayout>& hiZReduceLayout,
                                       const Ref<PipelineLayout>& hiZTailLayout);

        /// @brief Releases the SSR bindless slots; the images retire through the frame bin.
        ~SsrChain();

        SsrChain(const SsrChain&) = delete;
        SsrChain& operator=(const SsrChain&) = delete;

        /// @brief Recreates the scene-color or term targets, the mip chains, the min-Z pyramid, and the history.
        ///
        /// Allocates the targets and per-level descriptor sets when SSR runs (Settings.SSR or the
        /// Reflections debug arm); otherwise releases any previously-created ones. The min-Z pyramid
        /// mirrors the hi-Z reduction. Under a temporal resolve the scene-color intermediate gives way
        /// to the term and motion targets and the history pair, and the history is invalidated until
        /// a frame repopulates it. Called from the renderer's Create and every Resize/Configure.
        /// @param settings          The active renderer settings (SSR/Mode gate allocation, SsrResolutionScale sizes the chain).
        /// @param extent            The full render extent (the scene-color intermediate is full-res).
        /// @param historyExtent     The temporal resolve's output allocation, which the history covers.
        /// @param depthView         The live depth target the min-Z reduction's mip-0 source binds.
        /// @param hiZReduceSetLayout The hi-Z reduce set layout the min-Z reduce sets allocate from.
        /// @param hiZTailSetLayout  The hi-Z coarse-tail set layout the min-Z tail set allocates from.
        void Recreate(const SceneRendererSettings& settings, uvec2 extent, uvec2 historyExtent,
                      const Ref<ImageView>& depthView,
                      const Ref<DescriptorSetLayout>& hiZReduceSetLayout,
                      const Ref<DescriptorSetLayout>& hiZTailSetLayout);

        /// @brief Declares the min-Z reduction, trace, blur, and composite passes into the graph.
        ///
        /// Reduces this frame's depth to a min-Z pyramid, traces reflections against it into level 0
        /// of the reflection and hit-distance chains, low-passes both chains for rough reflections,
        /// then composites the reflection: into @p hdrId with no temporal resolve, or as the term and
        /// motion targets DeclareTemporal reads under one. Declared after the last pass writing the
        /// lit scene and ahead of the temporal resolve, or of the bloom sweep when no resolve runs.
        /// Imports the hit-distance and surface chains (and, under a temporal resolve, the term,
        /// motion and history targets) into the graph itself; their views bind through
        /// GetHitDistanceChainId, GetSurfaceChainId and AppendTemporalBindings.
        /// @param graph              The renderer's internal graph being rebuilt.
        /// @param sceneId            The lit scene color the trace reflects (and, with no temporal
        ///                           resolve, the composite adds onto).
        /// @param sceneHandle        Bindless slot for @p sceneId.
        /// @param reflectionChainId  The per-mip reflection pyramid import (trace + blur).
        /// @param hiZChainId         The per-mip min-Z pyramid import (reduction + trace).
        /// @param albedoId           The G0 albedo import (the composite's read, declared sampled).
        /// @param normalId           The G1 world-normal import (declared sampled for barrier order).
        /// @param ormId              The G2 packed occlusion/roughness/metallic import.
        /// @param depthId            The depth import.
        /// @param hdrId              The target the composite writes with no temporal resolve (unused under one).
        /// @param depthHandle        Bindless slot for the depth target.
        /// @param normalHandle       Bindless slot for the G1 normal.
        /// @param ormHandle          Bindless slot for the G2 ORM.
        /// @param albedoHandle       Bindless slot for the G0 albedo (the composite's metallic F0 tint).
        /// @param samplerHandle      Shared linear sampler bindless slot.
        void Declare(RenderGraph& graph, ResourceId sceneId, TextureHandle sceneHandle,
                     MipChainId reflectionChainId, MipChainId hiZChainId, ResourceId albedoId,
                     ResourceId normalId, ResourceId ormId, ResourceId depthId, ResourceId hdrId,
                     TextureHandle depthHandle, TextureHandle normalHandle, TextureHandle ormHandle,
                     TextureHandle albedoHandle, SamplerHandle samplerHandle);

        /// @brief Declares the temporal pass, accumulating the reflection term and adding it to the scene.
        ///
        /// Under a temporal resolve only, after the resolve's history copy has read @p sceneId, so the
        /// reflection never enters the resolve's history. Reconstructs the term at each output pixel's
        /// unjittered position, reprojects the chain's history by the reflected image's motion,
        /// validates and blends it, writes the next history, and adds the result onto @p sceneId.
        /// @param graph    The renderer's internal graph being rebuilt (Declare already ran on it).
        /// @param sceneId  The resolved scene colour, at the history's extent, the result is added onto.
        /// @pre Declare ran on @p graph with the chain allocated for a temporal resolve.
        void DeclareTemporal(RenderGraph& graph, ResourceId sceneId);

        /// @brief Appends the temporal targets' per-frame bindings: the term, the motion, and the history pairs.
        ///
        /// The history the pass reads and the one it writes swap each committed frame, so the pair is
        /// bound by this frame's parity. Appends nothing with no temporal resolve.
        /// @param bindings The renderer's import bindings for this Execute.
        void AppendTemporalBindings(std::vector<RenderGraph::ImportBinding>& bindings) const;

        /// @brief Records this frame's camera, from which the composite reprojects a reflected image.
        ///
        /// Composes, in double, the previous unjittered projection times rotation-only view applied to
        /// the camera's move since the last committed frame: with the view block's
        /// PrevFromCurDirection it carries a camera-relative point to the previous frame's clip space
        /// with no world-space translation in it.
        /// @param cameraPosition            This frame's camera world position.
        /// @param previousDirectionViewProj The previous frame's unjittered projection x rotation-only view.
        void SetCamera(const dvec3& cameraPosition, const dmat4& previousDirectionViewProj);

        /// @brief Commits this frame's history once its graph executed: the pair swaps, the reset clears, and the camera is kept.
        void CommitHistory();

        /// @brief The lit scene-color intermediate view (bound to its import with no temporal resolve).
        [[nodiscard]] const Ref<ImageView>& GetSceneView() const { return m_SceneView; }

        /// @brief Bindless slot for the scene-color intermediate (lighting's target with no temporal resolve).
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

        /// @brief The per-level hit-distance storage views, bound to their per-mip import slots each Execute.
        [[nodiscard]] const std::vector<Ref<ImageView>>& GetHitDistanceMipViews() const
        {
            return m_HitDistanceMips;
        }

        /// @brief The hit-distance chain's per-mip import, declared by the last Declare.
        [[nodiscard]] MipChainId GetHitDistanceChainId() const { return m_HitDistanceChainId; }

        /// @brief The per-level surface storage views, bound to their per-mip import slots each Execute.
        [[nodiscard]] const std::vector<Ref<ImageView>>& GetSurfaceMipViews() const
        {
            return m_SurfaceMips;
        }

        /// @brief The surface chain's per-mip import, declared by the last Declare.
        [[nodiscard]] MipChainId GetSurfaceChainId() const { return m_SurfaceChainId; }

        /// @brief The per-level min-Z storage views, bound to their per-mip import slots each Execute.
        [[nodiscard]] const std::vector<Ref<ImageView>>& GetHiZMipViews() const
        {
            return m_HiZMips;
        }

        /// @brief Number of min-Z mip levels (the import slot count and the chain binding range).
        [[nodiscard]] u32 GetHiZMipCount() const { return static_cast<u32>(m_HiZMips.size()); }

    private:
        SsrChain(Context& context, AssetManager& assets, const Ref<PipelineLayout>& hiZReduceLayout,
                 const Ref<PipelineLayout>& hiZTailLayout);

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
        /// @brief SSR composite pipeline (fullscreen, folds the reflection into the scene colour).
        Ref<GraphicsPipeline> m_CompositePipeline;
        /// @brief SSR composite's term pipeline (fullscreen, writes the reflection term and its motion).
        Ref<GraphicsPipeline> m_CompositeTermPipeline;
        /// @brief Layout for both composite pipelines (the composite push block).
        Ref<PipelineLayout> m_CompositeLayout;
        /// @brief SSR temporal pipeline (fullscreen, writes the next history and adds onto the scene).
        Ref<GraphicsPipeline> m_TemporalPipeline;
        /// @brief Layout for m_TemporalPipeline (the temporal push block).
        Ref<PipelineLayout> m_TemporalLayout;
        /// @brief SSR blur-downsample compute pipeline, low-passing both chains one level per dispatch.
        Ref<ComputePipeline> m_BlurPipeline;
        /// @brief The blur's set 3: both chains' sampled source and storage destination, and the sampler.
        Ref<DescriptorSetLayout> m_BlurSetLayout;
        /// @brief Layout for m_BlurPipeline (m_BlurSetLayout + the blur push block).
        Ref<PipelineLayout> m_BlurLayout;
        /// @brief SSR min-Z reduction compute pipeline (over the borrowed hi-Z reduce layout).
        Ref<ComputePipeline> m_HiZReducePipeline;
        /// @brief SSR min-Z coarse-tail compute pipeline (over the borrowed hi-Z tail layout).
        Ref<ComputePipeline> m_HiZTailPipeline;

        /// @brief Whether the chain resolves its own reflection over time (set by Recreate).
        bool m_Temporal = false;
        /// @brief The temporal resolve's output allocation the history covers (set by Recreate).
        uvec2 m_HistoryExtent{1};

        /// @brief Lit scene-color intermediate the trace samples and the composite adds onto (no temporal resolve).
        Ref<Image> m_SceneImage;
        /// @brief View over m_SceneImage.
        Ref<ImageView> m_SceneView;
        /// @brief Bindless slot for m_SceneView.
        TextureHandle m_SceneHandle;

        /// @brief Reflection mip chain, premultiplied by confidence: mip 0 the trace writes, coarser mips the blur produces.
        Ref<Image> m_ReflectionImage;
        /// @brief One single-mip view per reflection level (mip 0 a color target, deeper mips storage dests).
        std::vector<Ref<ImageView>> m_ReflectionMips;
        /// @brief Whole-chain sampled view of the reflection pyramid; the composite samples it by LOD.
        Ref<ImageView> m_ReflectionSampleView;
        /// @brief Bindless slot for m_ReflectionSampleView.
        TextureHandle m_ReflectionSampleHandle;
        /// @brief Hit-distance mip chain (R16F, premultiplied by confidence), the reflection chain's companion.
        Ref<Image> m_HitDistanceImage;
        /// @brief One single-mip view per hit-distance level (mip 0 a color target, deeper mips storage dests).
        std::vector<Ref<ImageView>> m_HitDistanceMips;
        /// @brief Whole-chain sampled view of the hit-distance chain; the composite reads it by level.
        Ref<ImageView> m_HitDistanceSampleView;
        /// @brief Bindless slot for m_HitDistanceSampleView.
        TextureHandle m_HitDistanceSampleHandle;
        /// @brief The hit-distance chain's per-mip import, declared by Declare.
        MipChainId m_HitDistanceChainId;
        /// @brief Surface mip chain (RGBA32F): each texel's surface device depth and its square, times its coverage, and the coverage.
        ///
        /// Level 0 the trace writes for every texel with a surface, whatever its ray did; the blur
        /// low-passes it with the reflection's own weights, so over its coverage a coarse texel
        /// holds the mean and spread of the surfaces its reflection averaged, which the composite
        /// weights it by.
        Ref<Image> m_SurfaceImage;
        /// @brief One single-mip view per surface level (mip 0 a color target, deeper mips storage dests).
        std::vector<Ref<ImageView>> m_SurfaceMips;
        /// @brief Whole-chain sampled view of the surface chain; the composite loads it by level.
        Ref<ImageView> m_SurfaceSampleView;
        /// @brief Bindless slot for m_SurfaceSampleView.
        TextureHandle m_SurfaceSampleHandle;
        /// @brief The surface chain's per-mip import, declared by Declare.
        MipChainId m_SurfaceChainId;
        /// @brief Trilinear clamp-to-edge sampler over both chains,
        /// shared out of the bindless registry with every other consumer of the same settings.
        Ref<Sampler> m_ReflectionSampler;
        /// @brief Bindless slot for m_ReflectionSampler; the registry's, so it stays valid.
        SamplerHandle m_ReflectionSamplerHandle;
        /// @brief One blur-downsample set per produced level k (reads the chains' mip k-1, writes their mip k).
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
        /// @brief The min-Z tail's set, or null when the pyramid has no tail.
        Ref<DescriptorSet> m_HiZTailSet;
        /// @brief The min-Z tail's first level (MipTailFirstLevel), meaningful while m_HiZTailSet is set.
        u32 m_HiZTailFirst = 0;

        /// @brief The reflection term the composite writes under a temporal resolve (render allocation).
        Ref<Image> m_TermImage;
        /// @brief View over m_TermImage.
        Ref<ImageView> m_TermView;
        /// @brief Bindless slot for m_TermView.
        TextureHandle m_TermHandle;
        /// @brief The term's import, declared by Declare.
        ResourceId m_TermId;
        /// @brief The reflected image's screen motion and view depth the composite writes beside the
        /// term.
        Ref<Image> m_MotionImage;
        /// @brief View over m_MotionImage.
        Ref<ImageView> m_MotionView;
        /// @brief Bindless slot for m_MotionView.
        TextureHandle m_MotionHandle;
        /// @brief The motion's import, declared by Declare.
        ResourceId m_MotionId;
        /// @brief The accumulated term and its sample count, ping-ponged across frames.
        std::array<Ref<Image>, 2> m_HistoryImages;
        /// @brief Views over m_HistoryImages.
        std::array<Ref<ImageView>, 2> m_HistoryViews;
        /// @brief Bindless slots for m_HistoryViews.
        std::array<TextureHandle, 2> m_HistoryHandles;
        /// @brief The import the temporal pass reads the previous history through, declared by Declare.
        ResourceId m_HistoryReadId;
        /// @brief The import the temporal pass writes the next history through, declared by Declare.
        ResourceId m_HistoryWriteId;
        /// @brief The history's fast luminance and its squared spread about the history, ping-ponged with it.
        ///
        /// The fast luminance accumulates about one jitter period and is clipped to the
        /// neighbourhood each frame, so it leaves the history within a frame of a real change while
        /// only rippling about it through a period; the history is doubted once it strays past its
        /// learnt spread.
        std::array<Ref<Image>, 2> m_FastImages;
        /// @brief Views over m_FastImages.
        std::array<Ref<ImageView>, 2> m_FastViews;
        /// @brief Bindless slots for m_FastViews.
        std::array<TextureHandle, 2> m_FastHandles;
        /// @brief The import the temporal pass reads the previous fast luminance through, declared by Declare.
        ResourceId m_FastReadId;
        /// @brief The import the temporal pass writes the next fast luminance through, declared by Declare.
        ResourceId m_FastWriteId;
        /// @brief Which of the pair this frame reads; the other is written.
        u32 m_HistoryParity = 0;
        /// @brief Forces the next temporal pass to ignore its history (set by Recreate).
        bool m_HistoryReset = true;
        /// @brief This frame's clip-space offset from the camera's move (SetCamera).
        vec4 m_PrevClipTranslation{0.0f};
        /// @brief This frame's camera world position (SetCamera).
        dvec3 m_CameraPosition{0.0};
        /// @brief The last committed frame's camera world position.
        dvec3 m_PreviousCameraPosition{0.0};
        /// @brief The shared linear sampler the temporal pass reads the history through, from Declare.
        SamplerHandle m_TemporalSamplerHandle;
    };
}
