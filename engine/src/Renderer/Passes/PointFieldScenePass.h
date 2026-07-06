#pragma once

#include <Veng/Veng.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/PointField.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/ScenePass.h>
#include <Veng/Renderer/Types.h>

namespace Veng
{
    class AssetManager;
}

namespace Veng::Renderer
{
    class Context;
    class GraphicsPipeline;
    class PipelineLayout;
    class DescriptorSet;
    class DescriptorSetLayout;
    class Buffer;

    /// @brief One GPU aggregate splat record (std430), matching the point_aggregate shader.
    ///
    /// A per-frame per-cell record consumed by the aggregate draw: the splat's world anchor plus
    /// its pixel kernel support, then the cell's flux-normalized per-pixel color (the summed cell
    /// flux spread over the splat's pixel area). Held in FieldState's reused staging vector.
    struct GpuAggregateSplat
    {
        /// @brief World-space anchor centroid (xyz) and quad kernel support in pixels (w).
        vec4 CenterSize;
        /// @brief Flux-normalized per-pixel color (rgb, HDR); w unused.
        vec4 Color;
    };

    /// @brief A contiguous resolved sprite draw: a buffer point range spanning one or more cells.
    ///
    /// Resolved cells tile the resident point buffer in ascending FirstPoint order (a Bucket
    /// postcondition), so a run of adjacent surviving cells is one point range [FirstPoint,
    /// FirstPoint+PointCount) drawn by a single sprite draw. A run breaks only where an
    /// intervening cell was culled or aggregated, so a wide view of a never-aggregating field
    /// collapses to one run over its whole in-frustum range.
    struct DrawRun
    {
        /// @brief Index of the run's first point in the resident buffer.
        u32 FirstPoint = 0;
        /// @brief Number of points in the run (summed over the merged cells).
        u32 PointCount = 0;
    };

    /// @brief Draws the scene's point fields into the linear HDR scene color, LOD-culled.
    ///
    /// Runs ahead of bloom and tonemap, accumulating over the lit HDR image with LoadOp::Load —
    /// a point field is an unlit emissive primitive whose additive radiance rolls off through the
    /// tone curve (instead of clipping in LDR) and whose bright points bloom like any other HDR
    /// emitter. Each Execute the pass draws every field the renderer resolved this frame: per
    /// field it CPU-frustum-culls the field's spatial cells against the camera, then per surviving
    /// cell estimates on-screen point density and routes the cell to one of two draws: the
    /// resolved sprites (camera-facing quads expanded from the cell's point range, pixel-clamped
    /// with flux-conserving brightness) below the density threshold, or a single additive density
    /// splat (the cell's summed flux spread over its projected footprint) above it — the two paths
    /// deliver the same integrated light, so the LOD transition holds brightness. Both pull points
    /// from the field's resident SSBO (set 1 binding 0) by SV_VertexID — no vertex input, no
    /// per-instance attribute — so the draw stays MoltenVK-clean (no drawIndirectCount, no
    /// base-instance capability).
    ///
    /// The fields are borrowed from the scene's PointField components (the renderer refills the set
    /// each Execute); an empty set makes the pass a per-frame no-op. Inserted only while a live
    /// field exists, so the shipping deferred path and the smoke golden are unchanged. Renders into
    /// the per-frame SceneView::RenderExtent sub-rect like every HDR pass, so it needs no Resize.
    class PointFieldScenePass final : public ScenePass
    {
    public:
        /// @brief Constructs the pass, building the sprite and aggregate pipelines.
        /// @param context        The render context.
        /// @param assets         Asset manager the point-field shaders load through (the core pack).
        /// @param fields         The renderer-owned live field set this Execute, refilled per frame.
        /// @param outputFormat   Color format of the HDR target the pass accumulates into.
        /// @param samplerHandle  Shared sampler bindless handle for the depth sample.
        /// @param framesInFlight Number of frame-in-flight ring regions for the aggregate records.
        PointFieldScenePass(Context& context, AssetManager& assets,
                            const vector<const PointField*>* fields, Format outputFormat,
                            SamplerHandle samplerHandle, u32 framesInFlight);

        /// @brief Destroys the pass's owned GPU resources.
        ~PointFieldScenePass() override;

        /// @brief Contributes the point-field pass into the graph.
        void Declare(RenderGraph& graph, const PassIO& io) override;

        /// @brief Returns the aggregate draw statistics from the most recent Execute.
        ///
        /// Refreshed at the end of every Execute (zeroed on a frame with no field draw), so a
        /// reader always sees a whole frame's counts. The pass fills a local block over the walk
        /// and publishes it once at the end, never a partially-accumulated one.
        /// @return The last Execute's point-field draw statistics.
        [[nodiscard]] const PointFieldStats& GetStats() const { return m_Stats; }

    private:
        /// @brief Per-field GPU state and hysteresis latch, cached across frames per drawn field.
        ///
        /// Each live field draws through its own descriptor sets and aggregate ring so concurrent
        /// per-frame writes never collide with a sibling field's pending draws in the same command
        /// buffer. Cached keyed by the field pointer (fields are long-lived resources) and pruned
        /// when a field is no longer resolved.
        struct FieldState
        {
            /// @brief One Set-1 point-buffer descriptor per frame-in-flight (sprite draw).
            ///
            /// Ringed like AggregateSets: when a field rebuilds its resident buffer, only the
            /// current frame's set is rewritten (its fence has been waited, so no pending command
            /// buffer references it); the other frames' sets re-point lazily as their turns come.
            /// A single shared set would be rewritten while a prior frame's draws still reference
            /// it — a validation error.
            vector<Ref<DescriptorSet>> SpriteSets;
            /// @brief Per-frame record of the buffer each SpriteSets entry points at.
            vector<const Buffer*> BoundBuffers;
            /// @brief One Set-1 aggregate descriptor per frame-in-flight, each pointing at its own
            /// ring region for the buffer's lifetime.
            ///
            /// The draw binds the current frame's set rather than rewriting a shared one each frame —
            /// updating a set a pending command buffer still references is a validation error.
            vector<Ref<DescriptorSet>> AggregateSets;
            /// @brief Host-mapped per-frame aggregate splat-record SSBO, ringed for frames-in-flight.
            Ref<Buffer> AggregateBuffer;
            /// @brief Byte stride between this field's aggregate-buffer ring regions.
            u64 AggregateRegionStride = 0;
            /// @brief Per-cell "currently aggregating" latch, keyed by cell index, for the LOD hysteresis.
            ///
            /// A cell's sprite<->aggregate choice retains across frames so the transition is
            /// hysteretic: a resolving cell flips to aggregate only once its density passes the high
            /// gate, and back only once it falls below the low gate. Sized to the field's cell count.
            vector<bool> Aggregating;
            /// @brief Merged contiguous sprite draw runs for this frame's walk.
            ///
            /// Cleared and refilled every Execute, reusing its capacity rather than reallocating
            /// a function-local vector per field per frame.
            vector<DrawRun> Runs;
            /// @brief Aggregate splat records staged for this frame's upload.
            ///
            /// Cleared and refilled every Execute, reusing its capacity across frames; the upload
            /// itself already rings correctly through AggregateBuffer.
            vector<GpuAggregateSplat> Splats;
            /// @brief Whether this field was resolved this Execute; unseen entries are pruned.
            bool Seen = false;
        };

        /// @brief Ensures a FieldState exists for @p field, allocating its sets and ring on first use.
        FieldState& StateFor(const PointField* field);

        /// @brief Ensures the shared quad index buffer holds at least @p quads quads' worth of indices.
        ///
        /// Both draw paths expand each point/splat into a quad through this one pass-owned index
        /// buffer (six indices per quad: 0,1,2, 1,3,2 offset by 4*q, u32). It grows on demand to
        /// the largest quad count any draw has needed and is rebuilt only on growth, retiring the
        /// old buffer through the per-frame deferred-destruction path so a pending draw is safe.
        /// @param quads Number of quads (points or splats) the next draw expands.
        void EnsureQuadIndexBuffer(u32 quads);

        /// @brief The render context.
        Context& m_Context;
        /// @brief Borrowed pointer to the renderer's live field set, refilled each Execute (may be empty).
        const vector<const PointField*>* m_Fields;
        /// @brief Number of frame-in-flight ring regions each field's aggregate buffer carries.
        u32 m_FramesInFlight;
        /// @brief HDR color format the pipelines target.
        Format m_OutputFormat;
        /// @brief Shared sampler bindless handle for the depth sample.
        SamplerHandle m_SamplerHandle;

        /// @brief Sprite pipeline (camera-facing quad expansion, additive blend) and its layout.
        Ref<GraphicsPipeline> m_SpritePipeline;
        /// @brief Aggregate pipeline (per-cell additive density splat) and its layout.
        Ref<GraphicsPipeline> m_AggregatePipeline;
        /// @brief Shared layout for both pipelines (set 0 bindless + set 1 point SSBO + push block).
        Ref<PipelineLayout> m_Layout;
        /// @brief Set-1 layout for the resident point SSBO (binding 0 storage buffer).
        Ref<DescriptorSetLayout> m_SetLayout;

        /// @brief Shared quad index buffer both draw paths index their point/splat quads through.
        ///
        /// Six u32 indices per quad (0,1,2, 1,3,2 offset by 4*q), grown on demand to the largest
        /// quad count any draw has needed and rebuilt only on growth. Null until the first draw.
        Ref<Buffer> m_QuadIndexBuffer;
        /// @brief Quad capacity the shared index buffer currently holds.
        u32 m_QuadIndexCapacity = 0;

        /// @brief Per-field GPU state and latches, keyed by field pointer, pruned when unresolved.
        unordered_map<const PointField*, FieldState> m_Fields_State;

        /// @brief Aggregate draw statistics from the most recent Execute; published at its end.
        PointFieldStats m_Stats;
    };
}
