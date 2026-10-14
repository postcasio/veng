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
    class GraphicsPipeline;
    class Image;
    class ImageView;
    class PipelineLayout;
    class Sampler;

    /// @brief The constants every pass of the reflection chain reads, one region per frame in
    ///        flight.
    ///
    /// The reference's cbSSSR, less the g-buffer decode parameters the engine's fixed g-buffer
    /// layout settles at the load, followed by the denoiser's render size, the one field of its
    /// cbDenoiserReflections the block does not already carry. Mirrors SsrConstants in
    /// ssr/ssr_common.slang field for field, under std140.
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
        /// @brief std140 alignment of DenoiserRenderSize.
        u32 Padding = 0;
        /// @brief The denoiser's render size: the extent the chain's resources are allocated at,
        ///        which the reference's host gives its denoiser as the context's window size.
        uvec2 DenoiserRenderSize{0};
        /// @brief One over DenoiserRenderSize.
        vec2 DenoiserInverseRenderSize{0.0f};
        /// @brief rgb the colour multiplying the environment the chain falls back to (the view's
        ///        EnvironmentLightTint); w unused.
        vec4 IblTint{1.0f, 1.0f, 1.0f, 0.0f};
    };
    static_assert(sizeof(SsrConstants) == 480, "SsrConstants must match ssr_common.slang (std140)");

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
        /// @brief The colour multiplying the environment the chain falls back to
        ///        (SceneView::EnvironmentLightTint).
        vec3 IblTint{1.0f};
        /// @brief Rays per 2×2 quad of glossy pixels (SceneView::SsrSamplesPerQuad).
        u32 SamplesPerQuad = 1;
        /// @brief SceneView::SsrRoughnessThreshold.
        f32 RoughnessThreshold = 0.2f;
        /// @brief SceneView::SsrVarianceThreshold.
        f32 VarianceThreshold = 0.0f;
        /// @brief SceneView::SsrVarianceGuidedTracing.
        bool VarianceGuidedTracing = true;
        /// @brief SceneView::SsrDepthBufferThickness.
        f32 DepthBufferThickness = 0.015f;
        /// @brief SceneView::SsrMaxTraversalIntersections.
        u32 MaxTraversalIntersections = 128;
        /// @brief SceneView::SsrMinTraversalOccupancy.
        u32 MinTraversalOccupancy = 4;
        /// @brief SceneView::SsrTemporalStabilityFactor.
        f32 TemporalStabilityFactor = 0.7f;
    };

    /// @brief The frame resources the reflection chain reads, as the graph and bindless see them.
    struct SsrGraphInputs
    {
        /// @brief The albedo target (G0), which the apply composite's Fresnel term reads.
        ResourceId Albedo;
        /// @brief The albedo target's bindless slot.
        TextureHandle AlbedoHandle;
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
        /// @brief This frame's lit opaque colour (HDR, after the deferred lighting and the sky), which
        ///        a ray's hit reads and the apply composite adds the reflection into.
        ResourceId Color;
        /// @brief The lit colour's bindless slot.
        TextureHandle ColorHandle;
        /// @brief The motion-vector target (G3), curUV - prevUV, which the denoiser's reprojection
        ///        reads.
        ResourceId Velocity;
        /// @brief The motion-vector target's bindless slot.
        TextureHandle VelocityHandle;
    };

    /// @brief Owns the screen-space reflection chain: its targets, lists, counters and passes.
    ///
    /// A port of FidelityFX SSSR and its reflection denoiser (SDK v1.1.4) onto the frame graph, its
    /// resources at the render allocation, created only while the renderer wires reflections. The
    /// passes it declares, in the reference's order: the depth hierarchy (one dispatch per level),
    /// the tile classifier, the blue-noise preparation, the indirect-argument preparation and the
    /// intersection (DeclareTrace), then the denoiser's reprojection, prefilter and temporal resolve
    /// and the history copies (DeclareDenoiser), then the sample's apply composite, which adds the
    /// denoised reflection into the lit colour (DeclareApply). The radiance, variance, sample-count and
    /// average-radiance targets are ping-ponged by the frame's parity, as the reference's hosts do,
    /// and every persistent target is cleared to zero whenever Recreate creates it, the reference's
    /// first-frame clear. After the temporal resolve, this frame's radiance A holds the denoised
    /// reflection. See the renderer's CLAUDE.md for the chain and its deviations from the reference.
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
        /// @brief The byte offset of the denoiser's dispatch in the indirect-argument buffer.
        static constexpr u32 DenoiserArgsOffset = 12;
        /// @brief The lit colour's format, which the apply composite's pipeline targets.
        static constexpr Format ApplyFormat = Format::RGBA16Sfloat;

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
        /// A changed extent recreates every one of them, clears every persistent target to zero
        /// and starts the frame count over, as the reference does when its context is recreated
        /// for a new size.
        /// @param extent The render allocation in pixels.
        void Recreate(uvec2 extent);

        /// @brief Has the classifier also write what it decided for each pixel, or stops it.
        ///
        /// On, the classifier runs its coverage entry point, writing one ssr_coverage.slang code per
        /// pixel to an R8 target at the render allocation (GetCoverageId); off, that pipeline and
        /// target are not held. A no-op when the state is unchanged.
        /// @param enabled Whether the coverage target is written.
        void SetCoverageOutput(bool enabled);

        /// @brief Points the rough pixels' environment read at a prefiltered specular cube.
        ///
        /// A no-op for the cube already bound.
        /// @param cube A cube view over every mip of the view's prefiltered specular probe.
        void SetEnvironment(const Ref<ImageView>& cube);

        /// @brief Points the apply composite at the split-sum BRDF LUT.
        ///
        /// A no-op for the LUT already bound.
        /// @param lut The view's BRDF integration LUT (x N·V, y perceptual roughness; rg the scale
        ///            and bias on F0), left sampled for the fragment stage.
        void SetBrdfLut(const Ref<ImageView>& lut);

        /// @brief Imports the chain's resources into the graph being rebuilt.
        /// @param graph The renderer's internal graph being rebuilt.
        /// @pre Recreate has run.
        void Import(RenderGraph& graph);

        /// @brief Declares the chain's passes into the graph, after the passes writing its inputs:
        ///        DeclareTrace, then DeclareDenoiser, then DeclareApply.
        /// @param graph  The graph Import declared the resources into.
        /// @param inputs The g-buffer and lit colour it reads, and the lit colour it composites into.
        /// @pre Import has run on @p graph, SetEnvironment has bound a cube and SetBrdfLut a LUT.
        void Declare(RenderGraph& graph, const SsrGraphInputs& inputs);

        /// @brief Declares the SSSR half of the chain: the depth hierarchy, the classifier, the
        ///        blue-noise and indirect-argument preparation and the intersection.
        ///
        /// Leaves the intersection's radiance in radiance A, which DeclareDenoiser replaces with the
        /// denoised reflection.
        /// @param graph  The graph Import declared the resources into.
        /// @param inputs The g-buffer and lit colour it reads.
        /// @pre Import has run on @p graph and SetEnvironment has bound a cube.
        void DeclareTrace(RenderGraph& graph, const SsrGraphInputs& inputs);

        /// @brief Declares the denoiser half of the chain: the reprojection, the prefilter, the
        ///        temporal resolve and the depth, normal and roughness history copies.
        ///
        /// Each of the three passes is one 8x8 workgroup per listed tile, dispatched indirectly
        /// from the argument buffer's second command.
        /// @param graph  The graph DeclareTrace declared into.
        /// @param inputs The g-buffer it reads; Normal must carry ImageUsage::TransferSrc.
        /// @pre DeclareTrace has run on @p graph.
        void DeclareDenoiser(RenderGraph& graph, const SsrGraphInputs& inputs);

        /// @brief Declares the apply composite: one fullscreen pass over the rendered sub-rect adding
        ///        `reflection · (F0·brdf.x + brdf.y)` into the lit colour.
        ///
        /// The blend is the sample's (colour One + Dst·SrcAlpha, alpha One + One), with the fragment
        /// writing alpha 1, so the reflection is added to what the colour holds.
        /// @param graph  The graph DeclareDenoiser declared into.
        /// @param inputs The g-buffer it reads; Color must carry ImageUsage::ColorAttachment in
        ///               SsrChain::ApplyFormat.
        /// @pre DeclareDenoiser has run on @p graph and SetBrdfLut has bound a LUT.
        void DeclareApply(RenderGraph& graph, const SsrGraphInputs& inputs);

        /// @brief Appends this frame's bindings for every resource Import declared.
        ///
        /// The ping-pong imports bind the targets this frame's parity selects.
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
        ///
        /// The intersection writes it, and the temporal resolve overwrites it with the denoised
        /// reflection: rgb the reflected radiance. Its alpha is the blue channel again on every pixel
        /// of a listed tile (the reference stores rgb.xyzz) and, elsewhere, the classifier's 0.
        [[nodiscard]] ResourceId GetRadianceId() const { return m_RadianceId; }

        /// @brief The import of the denoised reflection the temporal resolve writes: radiance A.
        [[nodiscard]] ResourceId GetReflectionId() const { return m_RadianceId; }

        /// @brief The bindless slot of this frame's radiance A, the view GetReflectionId binds.
        [[nodiscard]] TextureHandle GetReflectionHandle() const
        {
            return m_RadianceHandles[GetRadianceIndex()];
        }

        /// @brief The coverage target's import, declared by the last Import while SetCoverageOutput
        ///        is on; invalid otherwise.
        [[nodiscard]] ResourceId GetCoverageId() const { return m_CoverageId; }

        /// @brief The coverage target's bindless slot; invalid while SetCoverageOutput is off.
        [[nodiscard]] TextureHandle GetCoverageHandle() const { return m_CoverageHandle; }

        /// @brief The coverage target (R8, one ssr_coverage.slang code per pixel as code / 255);
        ///        null while SetCoverageOutput is off.
        [[nodiscard]] const Ref<ImageView>& GetCoverageView() const { return m_Coverage; }

        /// @brief The import this frame's radiance history (B) binds to: last frame's denoised
        ///        reflection, which the prefilter overwrites with its own output.
        [[nodiscard]] ResourceId GetRadianceHistoryId() const { return m_RadianceHistoryId; }

        /// @brief The import this frame's variance (A) binds to, which the reprojection and the
        ///        temporal resolve write and next frame's classifier reads.
        [[nodiscard]] ResourceId GetVarianceId() const { return m_VarianceId; }

        /// @brief The import this frame's variance history (B) binds to.
        [[nodiscard]] ResourceId GetVarianceHistoryId() const { return m_VarianceHistoryId; }

        /// @brief One of the two sample-count targets (R16F).
        /// @param index 0 or 1, the reference's SampleCount0 / SampleCount1.
        [[nodiscard]] const Ref<ImageView>& GetSampleCountView(u32 index) const
        {
            return m_SampleCount[index];
        }

        /// @brief The sample-count target this frame's reprojection writes.
        [[nodiscard]] u32 GetSampleCountIndex() const
        {
            return (m_FrameIndex & 1u) != 0u ? 0u : 1u;
        }

        /// @brief The sample-count target this frame reads: the one last frame's reprojection wrote.
        ///
        /// The temporal resolve reads it too, as the reference's host binds it.
        [[nodiscard]] u32 GetSampleCountHistoryIndex() const { return 1u - GetSampleCountIndex(); }

        /// @brief One of the two 8x8 average-radiance targets (B10G11R11, an eighth of the
        ///        allocation rounded up).
        /// @param index 0 or 1, the reference's AverageRadiance0 / AverageRadiance1.
        [[nodiscard]] const Ref<ImageView>& GetAverageRadianceView(u32 index) const
        {
            return m_AverageRadiance[index];
        }

        /// @brief The average-radiance target this frame's reprojection writes.
        [[nodiscard]] u32 GetAverageRadianceIndex() const
        {
            return (m_FrameIndex & 1u) != 0u ? 0u : 1u;
        }

        /// @brief The average-radiance target the prefilter and the temporal resolve read: the one
        ///        last frame's reprojection wrote, as the reference's host binds it.
        [[nodiscard]] u32 GetAverageRadianceHistoryIndex() const
        {
            return 1u - GetAverageRadianceIndex();
        }

        /// @brief The reprojected radiance (RGBA16F; rgb read).
        [[nodiscard]] const Ref<ImageView>& GetReprojectedRadianceView() const
        {
            return m_ReprojectedRadiance;
        }

        /// @brief The depth history: last frame's depth-hierarchy level 0 (R32F).
        [[nodiscard]] const Ref<ImageView>& GetDepthHistoryView() const { return m_DepthHistory; }

        /// @brief The normal history: last frame's world normal (the g-buffer's G1 format).
        [[nodiscard]] const Ref<ImageView>& GetNormalHistoryView() const { return m_NormalHistory; }

        /// @brief The roughness history: last frame's extracted roughness (R8, alpha).
        [[nodiscard]] const Ref<ImageView>& GetRoughnessHistoryView() const
        {
            return m_RoughnessHistory;
        }

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

        /// @brief Builds the three denoiser passes' sets for one frame parity.
        /// @param parity The frame index's parity (1 for odd frames).
        void BuildDenoiserSets(u32 parity);

        /// @brief The dynamic offset of this frame's constants region.
        [[nodiscard]] u32 ConstantsOffset() const;

        /// @brief Releases the bindless slots of the depth hierarchy's levels and the radiance pair.
        void ReleaseExtentHandles();

        /// @brief Creates the coverage target at the current extent, or drops it.
        /// @param enabled Whether the target is held.
        void RecreateCoverage(bool enabled);

        Context& m_Context;
        /// @brief The asset manager the coverage pipeline's shader loads through, on first use.
        AssetManager& m_Assets;

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
        /// @brief The classifier's coverage entry point (csCoverage), built on first use.
        Ref<ComputePipeline> m_CoveragePipeline;
        /// @brief Its layout: set 3 + the classifier's push block.
        Ref<PipelineLayout> m_CoverageLayout;
        /// @brief Its set 3: the classifier's bindings, then the coverage target.
        Ref<DescriptorSetLayout> m_CoverageSetLayout;
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
        /// @brief The intersection's pipeline.
        Ref<ComputePipeline> m_IntersectPipeline;
        /// @brief Its layout: set 3 + the normal and lit-colour push block.
        Ref<PipelineLayout> m_IntersectLayout;
        /// @brief Its set 3: constants, environment, depth hierarchy, extracted roughness, blue
        ///        noise, the ray list, the counters, radiance.
        Ref<DescriptorSetLayout> m_IntersectSetLayout;
        /// @brief The denoiser's reprojection pipeline.
        Ref<ComputePipeline> m_ReprojectPipeline;
        /// @brief Its layout: set 3 + the normal and motion-vector push block.
        Ref<PipelineLayout> m_ReprojectLayout;
        /// @brief Its set 3: constants, sampler, depth hierarchy, radiance A and B, variance B,
        ///        sample-count history, extracted roughness, the depth, normal and roughness
        ///        histories, then the variance A, sample-count, average and reprojected-radiance
        ///        targets and the tile list.
        Ref<DescriptorSetLayout> m_ReprojectSetLayout;
        /// @brief The denoiser's prefilter pipeline.
        Ref<ComputePipeline> m_PrefilterPipeline;
        /// @brief Its layout: set 3 + the normal push block.
        Ref<PipelineLayout> m_PrefilterLayout;
        /// @brief Its set 3: constants, sampler, depth hierarchy, radiance A, variance A,
        ///        average-radiance history, extracted roughness, then radiance B, variance B and
        ///        the tile list.
        Ref<DescriptorSetLayout> m_PrefilterSetLayout;
        /// @brief The denoiser's temporal-resolve pipeline.
        Ref<ComputePipeline> m_ResolvePipeline;
        /// @brief Its layout: set 3.
        Ref<PipelineLayout> m_ResolveLayout;
        /// @brief Its set 3: constants, sampler, radiance B, variance B, sample-count history,
        ///        average-radiance history, extracted roughness, reprojected radiance, then
        ///        radiance A, variance A and the tile list.
        Ref<DescriptorSetLayout> m_ResolveSetLayout;
        /// @brief The denoiser's sampler: bilinear, nearest mip, clamped.
        Ref<Sampler> m_DenoiserSampler;

        /// @brief The environment cube's sampler: trilinear, clamped in U and V, wrapped in W.
        Ref<Sampler> m_EnvironmentSampler;
        /// @brief The bound environment cube.
        Ref<ImageView> m_Environment;

        /// @brief The apply composite's pipeline (the fullscreen vertex stage, the sample's blend).
        Ref<GraphicsPipeline> m_ApplyPipeline;
        /// @brief Its layout: set 3 + the view direction and g-buffer push block.
        Ref<PipelineLayout> m_ApplyLayout;
        /// @brief Its set 3: the reflection, the BRDF LUT and the sample's linear sampler.
        Ref<DescriptorSetLayout> m_ApplySetLayout;
        /// @brief The sample's apply sampler: bilinear, nearest mip, clamped.
        Ref<Sampler> m_ApplySampler;
        /// @brief The bound BRDF LUT.
        Ref<ImageView> m_BrdfLut;
        /// @brief The camera's direction (its backward axis, towards the eye) PrepareFrame wrote,
        ///        the sample's per-frame viewDirection.
        vec3 m_ViewDirection{0.0f};

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
        /// @brief The radiance pair's bindless slots, which the reflection debug blit samples.
        std::array<TextureHandle, 2> m_RadianceHandles;
        /// @brief Whether the coverage target is wanted (SetCoverageOutput).
        bool m_CoverageWanted = false;
        /// @brief The coverage target (R8, one ssr_coverage.slang code per pixel), held only while
        ///        wanted.
        Ref<ImageView> m_Coverage;
        /// @brief The coverage target's bindless slot.
        TextureHandle m_CoverageHandle;
        /// @brief The variance ping-pong pair (R16F).
        std::array<Ref<ImageView>, 2> m_Variance;
        /// @brief The extracted roughness (R8, alpha).
        Ref<ImageView> m_ExtractedRoughness;
        /// @brief The sample-count ping-pong pair (R16F).
        std::array<Ref<ImageView>, 2> m_SampleCount;
        /// @brief The 8x8 average-radiance ping-pong pair (B10G11R11).
        std::array<Ref<ImageView>, 2> m_AverageRadiance;
        /// @brief The reprojected radiance (RGBA16F).
        Ref<ImageView> m_ReprojectedRadiance;
        /// @brief The depth history (R32F).
        Ref<ImageView> m_DepthHistory;
        /// @brief The normal history (the g-buffer's G1 format).
        Ref<ImageView> m_NormalHistory;
        /// @brief The roughness history (R8).
        Ref<ImageView> m_RoughnessHistory;
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
        /// @brief The coverage classifier's set for each frame parity: the classifier's, plus the
        ///        coverage target at binding 10. Built only while the coverage target is held.
        std::array<Ref<DescriptorSet>, 2> m_CoverageSets;
        /// @brief The blue-noise set.
        Ref<DescriptorSet> m_BlueNoiseSet;
        /// @brief The indirect-argument set.
        Ref<DescriptorSet> m_IndirectArgsSet;
        /// @brief The intersection's set for each frame parity (radiance A swaps).
        std::array<Ref<DescriptorSet>, 2> m_IntersectSets;
        /// @brief The reprojection's set for each frame parity.
        std::array<Ref<DescriptorSet>, 2> m_ReprojectSets;
        /// @brief The prefilter's set for each frame parity.
        std::array<Ref<DescriptorSet>, 2> m_PrefilterSets;
        /// @brief The temporal resolve's set for each frame parity.
        std::array<Ref<DescriptorSet>, 2> m_ResolveSets;
        /// @brief The apply composite's set for each frame parity (radiance A swaps).
        std::array<Ref<DescriptorSet>, 2> m_ApplySets;

        /// @brief The depth hierarchy's per-level import.
        MipChainId m_DepthHierarchyId;
        /// @brief This frame's radiance (A) import.
        ResourceId m_RadianceId;
        /// @brief The coverage target's import (invalid while it is not held).
        ResourceId m_CoverageId;
        /// @brief This frame's radiance history (B) import.
        ResourceId m_RadianceHistoryId;
        /// @brief This frame's variance (A) import.
        ResourceId m_VarianceId;
        /// @brief This frame's variance history (B) import.
        ResourceId m_VarianceHistoryId;
        /// @brief The sample-count target this frame writes.
        ResourceId m_SampleCountId;
        /// @brief The sample-count target this frame reads.
        ResourceId m_SampleCountHistoryId;
        /// @brief The average-radiance target this frame writes.
        ResourceId m_AverageRadianceId;
        /// @brief The average-radiance target this frame reads.
        ResourceId m_AverageRadianceHistoryId;
        /// @brief The reprojected radiance import.
        ResourceId m_ReprojectedRadianceId;
        /// @brief The depth history import.
        ResourceId m_DepthHistoryId;
        /// @brief The normal history import.
        ResourceId m_NormalHistoryId;
        /// @brief The roughness history import.
        ResourceId m_RoughnessHistoryId;
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
