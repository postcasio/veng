#pragma once

#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Veng.h>

#include <array>

namespace Veng
{
    class AssetManager;
}

namespace Veng::Renderer
{
    class Buffer;
    class ComputePipeline;
    class Context;
    class DescriptorSet;
    class DescriptorSetLayout;
    class Image;
    class ImageView;
    class PipelineLayout;
    class Sampler;

    /// @brief The constants every pass of the reflection chain reads, one region per frame in
    ///        flight.
    ///
    /// The reference's cbSSSR, less the g-buffer decode parameters the engine's fixed g-buffer
    /// layout settles at the load. Mirrors SsrConstants in ssr/ssr_common.slang field for field,
    /// under std140.
    struct SsrConstants
    {
        /// @brief Inverse of the rasterized view-projection.
        mat4 InvViewProjection;
        /// @brief The rasterized (jittered) projection.
        mat4 Projection;
        /// @brief Inverse of Projection.
        mat4 InvProjection;
        /// @brief World to view.
        mat4 ViewMatrix;
        /// @brief View to world.
        mat4 InvView;
        /// @brief The previous frame's unjittered view-projection.
        mat4 PrevViewProjection;
        /// @brief The rendered extent in pixels (the dynamic-resolution sub-rect).
        uvec2 RenderSize{0};
        /// @brief One over RenderSize.
        vec2 InverseRenderSize{0.0f};
        /// @brief Scale on the environment the chain falls back to.
        f32 IblFactor = 0.0f;
        /// @brief The denoiser's history weight.
        f32 TemporalStabilityFactor = 0.0f;
        /// @brief The intersection's depth-buffer thickness.
        f32 DepthBufferThickness = 0.0f;
        /// @brief The GGX alpha below which a pixel traces.
        f32 RoughnessThreshold = 0.0f;
        /// @brief The history variance above which a skipped pixel traces.
        f32 VarianceThreshold = 0.0f;
        /// @brief The chain's frame count since its resources were created.
        u32 FrameIndex = 0;
        /// @brief The intersection's iteration bound.
        u32 MaxTraversalIntersections = 0;
        /// @brief The subgroup occupancy below which the intersection stops early.
        u32 MinTraversalOccupancy = 0;
        /// @brief The depth-hierarchy level the intersection descends to.
        u32 MostDetailedMip = 0;
        /// @brief Rays per 2×2 quad of glossy pixels.
        u32 SamplesPerQuad = 1;
        /// @brief Non-zero when variance-guided tracing is on.
        u32 TemporalVarianceGuidedTracingEnabled = 0;
        /// @brief std140 tail padding.
        u32 Padding = 0;
    };
    static_assert(sizeof(SsrConstants) == 448, "SsrConstants must match ssr_common.slang (std140)");

    /// @brief The view-dependent inputs a frame of the reflection chain runs with.
    struct SsrFrameInputs
    {
        /// @brief World to view.
        mat4 View{1.0f};
        /// @brief The projection the frame was rasterized through, jitter included.
        mat4 Projection{1.0f};
        /// @brief The previous frame's unjittered view-projection.
        mat4 PrevViewProjection{1.0f};
        /// @brief The rendered extent in pixels.
        uvec2 RenderSize{0};
        /// @brief The view's image-based-lighting intensity, or 0 for a view without image-based
        ///        lighting.
        f32 IblFactor = 0.0f;
        /// @brief Rays per 2×2 quad of glossy pixels (SceneView::SsrSamplesPerQuad).
        u32 SamplesPerQuad = 1;
        /// @brief SceneView::SsrRoughnessThreshold.
        f32 RoughnessThreshold = 0.2f;
        /// @brief SceneView::SsrVarianceThreshold.
        f32 VarianceThreshold = 0.0f;
        /// @brief SceneView::SsrVarianceGuidedTracing.
        bool VarianceGuidedTracing = true;
    };

    /// @brief The g-buffer resources the reflection chain reads, as the graph and bindless see them.
    struct SsrGraphInputs
    {
        /// @brief The depth target.
        ResourceId Depth;
        /// @brief The world-normal target (G1).
        ResourceId Normal;
        /// @brief The occlusion/roughness/metallic target (G2).
        ResourceId Material;
        /// @brief The depth target's bindless slot.
        TextureHandle DepthHandle;
        /// @brief The world-normal target's bindless slot.
        TextureHandle NormalHandle;
        /// @brief The occlusion/roughness/metallic target's bindless slot.
        TextureHandle MaterialHandle;
    };

    /// @brief Owns the screen-space reflection chain: its targets, lists, counters and passes.
    ///
    /// A port of FidelityFX SSSR (SDK v1.1.4) onto the frame graph, its resources at the render
    /// allocation, created only while the renderer wires reflections. The passes it declares, in
    /// the reference's order: the depth hierarchy (one dispatch per level), the tile classifier,
    /// the blue-noise preparation and the indirect-argument preparation. The radiance and variance
    /// targets are ping-ponged by the frame's parity, as the reference's host does, and are cleared
    /// to zero whenever Recreate creates them, the reference's first-frame clear. See the renderer's
    /// CLAUDE.md for the chain and its deviations from the reference.
    class SsrChain
    {
    public:
        /// @brief The depth hierarchy's level count at most: the copy of the depth and six
        ///        reductions.
        static constexpr u32 MaxDepthHierarchyLevels = 7;
        /// @brief The blue-noise tile's edge in texels.
        static constexpr u32 BlueNoiseSize = 128;
        /// @brief Words in the counter buffer: rays found, rays to trace, tiles found, tiles to
        ///        denoise.
        static constexpr u32 CounterWords = 4;
        /// @brief Words in the indirect-argument buffer: the intersection's dispatch, then the
        ///        denoiser's.
        static constexpr u32 IndirectArgsWords = 6;

        /// @brief Loads the shaders and the sampler tables and builds the pipelines.
        ///
        /// Allocates no extent-sized resource until Recreate.
        /// @param context The render context the resources are created on.
        /// @param assets  Asset manager the shaders and tables load through.
        /// @return A new SsrChain.
        /// @pre The device runs basic and ballot subgroup operations in compute (asserted).
        static Unique<SsrChain> Create(Context& context, AssetManager& assets);

        /// @brief Releases the chain's bindless slots; its resources retire deferred.
        ~SsrChain();

        SsrChain(const SsrChain&) = delete;
        SsrChain& operator=(const SsrChain&) = delete;

        /// @brief Creates the extent-sized resources for a render allocation, or keeps them.
        ///
        /// A changed extent recreates every one of them, clears the radiance and variance targets
        /// to zero and starts the frame count over, as the reference does when its context is
        /// recreated for a new size.
        /// @param extent The render allocation in pixels.
        void Recreate(uvec2 extent);

        /// @brief Points the rough pixels' environment read at a prefiltered specular cube.
        ///
        /// A no-op for the cube already bound.
        /// @param cube A cube view over every mip of the view's prefiltered specular probe.
        void SetEnvironment(const Ref<ImageView>& cube);

        /// @brief Imports the chain's resources into the graph being rebuilt.
        /// @param graph The renderer's internal graph being rebuilt.
        /// @pre Recreate has run.
        void Import(RenderGraph& graph);

        /// @brief Declares the chain's passes into the graph, after the passes writing its inputs.
        /// @param graph  The graph Import declared the resources into.
        /// @param inputs The g-buffer it reads.
        /// @pre Import has run on @p graph and SetEnvironment has bound a cube.
        void Declare(RenderGraph& graph, const SsrGraphInputs& inputs);

        /// @brief Appends this frame's bindings for every resource Import declared.
        ///
        /// The radiance and variance imports bind the targets this frame's parity selects.
        /// @param bindings The frame's import bindings.
        void AppendImportBindings(vector<RenderGraph::ImportBinding>& bindings) const;

        /// @brief Writes this frame's constants into the frame-in-flight's region.
        /// @param inputs The view-dependent inputs.
        void PrepareFrame(const SsrFrameInputs& inputs);

        /// @brief Counts a frame, flipping the ping-pong parity for the next one.
        void AdvanceFrame() { ++m_FrameIndex; }

        /// @brief The frames run since the resources were created.
        [[nodiscard]] u32 GetFrameIndex() const { return m_FrameIndex; }

        /// @brief The render allocation the resources are sized to.
        [[nodiscard]] uvec2 GetExtent() const { return m_Extent; }

        /// @brief The depth hierarchy (R32F, the render allocation, GetDepthHierarchyLevels levels).
        [[nodiscard]] const Ref<Image>& GetDepthHierarchy() const { return m_DepthHierarchy; }

        /// @brief The depth hierarchy's level count.
        [[nodiscard]] u32 GetDepthHierarchyLevels() const
        {
            return static_cast<u32>(m_DepthHierarchyLevels.size());
        }

        /// @brief One level of the depth hierarchy, as a single-level view.
        [[nodiscard]] const Ref<ImageView>& GetDepthHierarchyLevel(u32 level) const
        {
            return m_DepthHierarchyLevels[level];
        }

        /// @brief The whole depth hierarchy as one sampled view.
        [[nodiscard]] const Ref<ImageView>& GetDepthHierarchyView() const
        {
            return m_DepthHierarchyView;
        }

        /// @brief The depth hierarchy's per-level graph import, declared by the last Import.
        [[nodiscard]] MipChainId GetDepthHierarchyId() const { return m_DepthHierarchyId; }

        /// @brief One of the two radiance targets (RGBA16F).
        /// @param index 0 or 1, the reference's Radiance0 / Radiance1.
        [[nodiscard]] const Ref<ImageView>& GetRadianceView(u32 index) const
        {
            return m_Radiance[index];
        }

        /// @brief One of the two variance targets (R16F).
        /// @param index 0 or 1, the reference's Variance0 / Variance1.
        [[nodiscard]] const Ref<ImageView>& GetVarianceView(u32 index) const
        {
            return m_Variance[index];
        }

        /// @brief The radiance target this frame writes (the reference's "radiance A").
        [[nodiscard]] u32 GetRadianceIndex() const { return (m_FrameIndex & 1u) != 0u ? 0u : 1u; }

        /// @brief The radiance target this frame reads as history (the reference's "radiance B").
        [[nodiscard]] u32 GetRadianceHistoryIndex() const { return 1u - GetRadianceIndex(); }

        /// @brief The variance target this frame writes (the reference's "variance A").
        [[nodiscard]] u32 GetVarianceIndex() const { return (m_FrameIndex & 1u) != 0u ? 0u : 1u; }

        /// @brief The variance target this frame reads as history (the reference's "variance B").
        [[nodiscard]] u32 GetVarianceHistoryIndex() const { return 1u - GetVarianceIndex(); }

        /// @brief The import this frame's radiance (A) binds to, declared by the last Import.
        [[nodiscard]] ResourceId GetRadianceId() const { return m_RadianceId; }

        /// @brief The import this frame's variance history (B) binds to.
        [[nodiscard]] ResourceId GetVarianceHistoryId() const { return m_VarianceHistoryId; }

        /// @brief The classifier's extracted roughness, in alpha (R8).
        [[nodiscard]] const Ref<ImageView>& GetExtractedRoughnessView() const
        {
            return m_ExtractedRoughness;
        }

        /// @brief The extracted roughness's graph import.
        [[nodiscard]] ResourceId GetExtractedRoughnessId() const { return m_ExtractedRoughnessId; }

        /// @brief The blue-noise tile (RG8, BlueNoiseSize²).
        [[nodiscard]] const Ref<ImageView>& GetBlueNoiseView() const { return m_BlueNoise; }

        /// @brief The blue-noise tile's graph import.
        [[nodiscard]] ResourceId GetBlueNoiseId() const { return m_BlueNoiseId; }

        /// @brief The ray list: one packed word per ray (x 15 bits, y 14 bits, copy flags 29–31).
        [[nodiscard]] const Ref<Buffer>& GetRayList() const { return m_RayList; }

        /// @brief The ray list's graph import.
        [[nodiscard]] ResourceId GetRayListId() const { return m_RayListId; }

        /// @brief The denoiser tile list: one word per tile, its first pixel as x | y << 16.
        [[nodiscard]] const Ref<Buffer>& GetDenoiserTileList() const { return m_DenoiserTileList; }

        /// @brief The denoiser tile list's graph import.
        [[nodiscard]] ResourceId GetDenoiserTileListId() const { return m_DenoiserTileListId; }

        /// @brief The counters (CounterWords words).
        [[nodiscard]] const Ref<Buffer>& GetRayCounter() const { return m_RayCounter; }

        /// @brief The counters' graph import.
        [[nodiscard]] ResourceId GetRayCounterId() const { return m_RayCounterId; }

        /// @brief The indirect arguments (IndirectArgsWords words).
        [[nodiscard]] const Ref<Buffer>& GetIndirectArgs() const { return m_IndirectArgs; }

        /// @brief The indirect arguments' graph import.
        [[nodiscard]] ResourceId GetIndirectArgsId() const { return m_IndirectArgsId; }

        /// @brief The constants ring (one UniformBufferDynamic region per frame in flight).
        [[nodiscard]] const Ref<Buffer>& GetConstants() const { return m_Constants; }

        /// @brief The byte stride between the constants ring's regions.
        [[nodiscard]] u32 GetConstantsStride() const { return m_ConstantsStride; }

    private:
        SsrChain(Context& context, AssetManager& assets);

        /// @brief Builds the per-extent descriptor sets against the current resources.
        void BuildSets();

        /// @brief The dynamic offset of this frame's constants region.
        [[nodiscard]] u32 ConstantsOffset() const;

        /// @brief Releases the per-level bindless slots of the depth hierarchy.
        void ReleaseHierarchyHandles();

        Context& m_Context;

        /// @brief The render allocation the resources are sized to (zero before Recreate).
        uvec2 m_Extent{0};
        /// @brief Frames run since the resources were created; its parity picks the ping-pong
        ///        targets.
        u32 m_FrameIndex = 0;
        /// @brief The rendered extent the last PrepareFrame wrote, which the dispatches cover.
        uvec2 m_RenderSize{0};

        /// @brief The depth hierarchy's pipeline (one level per dispatch).
        Ref<ComputePipeline> m_DownsamplePipeline;
        /// @brief Its layout: set 3 (the destination level) + the push block.
        Ref<PipelineLayout> m_DownsampleLayout;
        /// @brief Its set 3: one storage destination level.
        Ref<DescriptorSetLayout> m_DownsampleSetLayout;
        /// @brief The classifier's pipeline.
        Ref<ComputePipeline> m_ClassifyPipeline;
        /// @brief Its layout: set 3 + the g-buffer push block.
        Ref<PipelineLayout> m_ClassifyLayout;
        /// @brief Its set 3: constants, environment, depth hierarchy, variance history, radiance,
        ///        the two lists, the counters, extracted roughness.
        Ref<DescriptorSetLayout> m_ClassifySetLayout;
        /// @brief The blue-noise pipeline.
        Ref<ComputePipeline> m_BlueNoisePipeline;
        /// @brief Its layout: set 3.
        Ref<PipelineLayout> m_BlueNoiseLayout;
        /// @brief Its set 3: constants, the two sampler tables, the blue-noise tile.
        Ref<DescriptorSetLayout> m_BlueNoiseSetLayout;
        /// @brief The indirect-argument pipeline.
        Ref<ComputePipeline> m_IndirectArgsPipeline;
        /// @brief Its layout: set 3.
        Ref<PipelineLayout> m_IndirectArgsLayout;
        /// @brief Its set 3: the counters and the arguments.
        Ref<DescriptorSetLayout> m_IndirectArgsSetLayout;

        /// @brief The environment cube's sampler: trilinear, clamped in U and V, wrapped in W.
        Ref<Sampler> m_EnvironmentSampler;
        /// @brief The bound environment cube.
        Ref<ImageView> m_Environment;

        /// @brief The constants ring, host-mapped.
        Ref<Buffer> m_Constants;
        /// @brief The ring's region stride, a multiple of the uniform-buffer offset alignment.
        u32 m_ConstantsStride = 0;

        /// @brief The Sobol sequence (R32_UINT 256×256).
        Ref<ImageView> m_Sobol;
        /// @brief The scrambling tile (R32_UINT 512×256).
        Ref<ImageView> m_ScramblingTile;
        /// @brief The blue-noise tile (RG8 BlueNoiseSize²).
        Ref<ImageView> m_BlueNoise;

        /// @brief The depth hierarchy image.
        Ref<Image> m_DepthHierarchy;
        /// @brief One single-level view per depth-hierarchy level.
        vector<Ref<ImageView>> m_DepthHierarchyLevels;
        /// @brief The bindless slot of each level's view, which the next level reduces from.
        vector<TextureHandle> m_DepthHierarchyHandles;
        /// @brief The whole hierarchy as one sampled view.
        Ref<ImageView> m_DepthHierarchyView;
        /// @brief The radiance ping-pong pair (RGBA16F).
        std::array<Ref<ImageView>, 2> m_Radiance;
        /// @brief The variance ping-pong pair (R16F).
        std::array<Ref<ImageView>, 2> m_Variance;
        /// @brief The extracted roughness (R8, alpha).
        Ref<ImageView> m_ExtractedRoughness;
        /// @brief The ray list (one word per pixel of the allocation).
        Ref<Buffer> m_RayList;
        /// @brief The denoiser tile list (one word per pixel of the allocation, as the reference).
        Ref<Buffer> m_DenoiserTileList;
        /// @brief The counters (CounterWords words, zeroed at creation).
        Ref<Buffer> m_RayCounter;
        /// @brief The indirect arguments (IndirectArgsWords words).
        Ref<Buffer> m_IndirectArgs;

        /// @brief One destination set per depth-hierarchy level.
        vector<Ref<DescriptorSet>> m_DownsampleSets;
        /// @brief The classifier's set for each frame parity (radiance A and variance B swap).
        std::array<Ref<DescriptorSet>, 2> m_ClassifySets;
        /// @brief The blue-noise set.
        Ref<DescriptorSet> m_BlueNoiseSet;
        /// @brief The indirect-argument set.
        Ref<DescriptorSet> m_IndirectArgsSet;

        /// @brief The depth hierarchy's per-level import.
        MipChainId m_DepthHierarchyId;
        /// @brief This frame's radiance (A) import.
        ResourceId m_RadianceId;
        /// @brief This frame's variance history (B) import.
        ResourceId m_VarianceHistoryId;
        /// @brief The extracted roughness import.
        ResourceId m_ExtractedRoughnessId;
        /// @brief The blue-noise import.
        ResourceId m_BlueNoiseId;
        /// @brief The ray list import.
        ResourceId m_RayListId;
        /// @brief The denoiser tile list import.
        ResourceId m_DenoiserTileListId;
        /// @brief The counters import.
        ResourceId m_RayCounterId;
        /// @brief The indirect arguments import.
        ResourceId m_IndirectArgsId;
    };
}
