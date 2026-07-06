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
    class ComputePipeline;
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

    /// @brief One resolved run uploaded to the expansion compute pass (std430, matches GpuDrawRun).
    ///
    /// The GPU form of a DrawRun: the point range plus the exclusive prefix sum of every earlier
    /// run's point count, so a compute thread maps to its point by a prefix search over the run
    /// table. Uploaded per frame into the field's ring-buffered run buffer.
    struct GpuDrawRun
    {
        /// @brief Index of the run's first point in the resident buffer.
        u32 FirstPoint = 0;
        /// @brief Number of points in the run.
        u32 Count = 0;
        /// @brief Points in every run before this one (exclusive prefix sum), the thread-to-run key.
        u32 PointPrefix = 0;
        /// @brief Pad to a 16-byte std430 stride.
        u32 Pad = 0;
    };

    /// @brief One compacted per-point sprite record the expansion pass emits (32 B, matches the shader).
    ///
    /// The expansion compute pass runs the per-point sprite math once and writes one of these per
    /// surviving point; the sprite vertex stage fetches it and applies the corner FMA — no matrix,
    /// no clamp, no unpack in the raster path. HalfExtents and Color pack their data as f16 pairs:
    /// the direct path's own interpolants are already f16-class on the target hardware, so the
    /// quantization sits below the photometric thresholds the two paths are compared against.
    struct GpuSpriteRecord
    {
        /// @brief Projected sprite center in clip space (pre-divide).
        vec4 ClipCenter;
        /// @brief Clip-space half-extent xy: the right offset (2×f16) and the up offset (2×f16).
        uvec2 HalfExtents;
        /// @brief Folded HDR color: rg (2×f16) and b + an occluded-fade flag (2×f16).
        uvec2 Color;
    };

    static_assert(sizeof(GpuSpriteRecord) == 32,
                  "GpuSpriteRecord must be 32 bytes (matches shader)");

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
    /// deliver the same integrated light, so the LOD transition holds brightness.
    ///
    /// The resolved sprites take one of two per-field paths, selected automatically and reported
    /// through PointFieldStats::DrawSource. The compute path runs the per-point sprite work once in
    /// an expansion dispatch — projecting, clamping, and folding each point into a compact record,
    /// compacting out zero-contribution points — then draws the survivors through one
    /// DrawIndexedIndirect per field (a single GPU-written command; no drawIndirectCount, no
    /// multiDrawIndirect — the MoltenVK-supported form). The direct path expands every point in the
    /// vertex stage from the resident SSBO, the fallback for a device without the compute path, the
    /// A/B verification reference, and the first frame after a field rebuild. Both index quads by
    /// SV_VertexID — no vertex input, no per-instance attribute, no base-instance capability.
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

        /// @brief Forces every field onto the direct sprite path, bypassing the compute expansion.
        ///
        /// The compute path and the direct path draw a surviving point bit-comparably (modulo the
        /// record's f16 quantization), so forcing direct is the A/B verification reference against
        /// the automatic per-field selection. On a device without the compute path's features every
        /// field already takes the direct path, so this is a no-op there.
        /// @param force True to draw every field direct; false to restore automatic selection.
        void SetForceDirect(bool force) { m_ForceDirect = force; }

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

            /// @brief Device-local per-point sprite-record buffer, ringed framesInFlight deep.
            ///
            /// The expansion compute pass writes one GpuSpriteRecord per surviving point into the
            /// current frame's region; the sprite vertex stage reads it. Capacity is the field's
            /// point count (a survivor count never exceeds it), so the ring mirrors the aggregate
            /// buffer's per-field, per-frame shape at 32 B/point per region. Null until the compute
            /// path is first taken for this field.
            Ref<Buffer> RecordBuffer;
            /// @brief Byte stride between this field's record-buffer ring regions.
            u64 RecordRegionStride = 0;
            /// @brief One Set-1 compute descriptor per frame-in-flight (points + runs + record + args).
            ///
            /// Ringed like SpriteSets: written on demand to the current frame's region so a pending
            /// frame's dispatch never has its set rewritten under it. Bound by the expansion dispatch.
            vector<Ref<DescriptorSet>> ComputeSets;
            /// @brief One Set-1 sprite descriptor per frame-in-flight bound at the record region.
            ///
            /// The compute path's analogue of SpriteSets: it points the sprite vertex stage at the
            /// record buffer's current ring region rather than the resident point buffer.
            vector<Ref<DescriptorSet>> RecordSets;
            /// @brief Host-mapped run table, ringed framesInFlight deep, uploaded per frame.
            ///
            /// Holds this frame's GpuDrawRun run list for the compute dispatch's prefix search.
            /// Capacity is the field's cell count (a run never exceeds one per cell).
            Ref<Buffer> RunBuffer;
            /// @brief Byte stride between this field's run-buffer ring regions.
            u64 RunRegionStride = 0;
            /// @brief Host-mapped indirect-args buffer, ringed framesInFlight deep.
            ///
            /// One VkDrawIndexedIndirectCommand per frame region. The CPU presets the fixed fields
            /// and zeroes indexCount before the dispatch; the compute pass finalizes indexCount to
            /// 6·survivors. Host-mapped so the CPU reads back the prior frame's index count for the
            /// CompactedPoints stat.
            Ref<Buffer> ArgsBuffer;
            /// @brief Byte stride between this field's args-buffer ring regions.
            u64 ArgsRegionStride = 0;
            /// @brief Host-mapped atomic append cursor, one uint per frame region.
            ///
            /// Separate from ArgsBuffer so the compute pass's g_Cursor storage buffer does not alias
            /// the command's first word (both descriptors base at their region offset). Zeroed by the
            /// CPU before each dispatch; the compute pass appends survivors through it.
            Ref<Buffer> CursorBuffer;
            /// @brief Byte stride between this field's cursor-buffer ring regions.
            u64 CursorRegionStride = 0;
            /// @brief Number of record-buffer slots the ring currently holds (the field's point count).
            u32 RecordCapacity = 0;

            /// @brief Whether this Execute's resolved sprites drew through the compute path.
            bool DrewCompute = false;
            /// @brief This Execute's sprite point total (pre-compaction), for the compacted-out stat.
            u64 SpritePointTotal = 0;
            /// @brief Survivor count read from this region's prior compute draw (one frame late).
            ///
            /// Read from the indirect command's index count before it is reset each Execute, so the
            /// CompactedPoints stat reports submitted minus survivors without a GPU stall.
            u64 PriorSurvivors = 0;
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

        /// @brief Allocates a field's compute record ring, run/args buffers, and sets on first use.
        ///
        /// The compute path's per-field GPU state, sized once to the field's point/cell counts and
        /// ringed framesInFlight deep. A no-op after the first allocation; a rebuilt field that
        /// outgrows its record capacity is caught by the caller (which falls back to direct that
        /// frame) rather than reallocated mid-Execute.
        /// @param state The field's cached state.
        /// @param field The field being drawn.
        void EnsureComputeResources(FieldState& state, const PointField* field);

        /// @brief Runs the per-field CPU walk and records the compute expansion dispatches.
        ///
        /// The compute pass's Execute body: for every live field it frustum-culls and LOD-partitions
        /// its cells (filling FieldState's reused Runs/Splats), stages the aggregate records, and —
        /// for a field on the compute path — uploads its run table and records the expansion dispatch
        /// plus the compute-write → vertex/indirect-read buffer barrier. It publishes the walk-side
        /// stats; DrawFields adds the submission counters. Runs ahead of DrawFields in the graph.
        /// @param cmd  The frame's command buffer (outside any render pass here).
        /// @param ctx  The scene pass context (view + resolved resources).
        void WalkFields(CommandBuffer& cmd, const ScenePassContext& ctx);

        /// @brief Draws every field's aggregate splats and resolved sprites (indirect or direct).
        ///
        /// The graphics pass's Execute body, reading the plan WalkFields left in each FieldState:
        /// the aggregate splats (one draw), then the resolved sprites through either one indirect
        /// draw per field (compute path) or one indexed draw per run (direct path). Finalizes and
        /// publishes m_Stats, prunes unseen fields.
        /// @param cmd  The frame's command buffer.
        /// @param ctx  The scene pass context.
        void DrawFields(CommandBuffer& cmd, const ScenePassContext& ctx);

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

        /// @brief Compute-path sprite pipeline: its vertex stage fetches a record and applies one FMA.
        Ref<GraphicsPipeline> m_SpritePipeline;
        /// @brief Direct-path sprite pipeline: its vertex stage reads the resident points per corner.
        ///
        /// The fallback for a device without the compute path and the A/B verification reference; it
        /// draws from the resident point SSBO (SpriteSets) rather than the compacted record buffer.
        Ref<GraphicsPipeline> m_SpriteDirectPipeline;
        /// @brief Aggregate pipeline (per-cell additive density splat) and its layout.
        Ref<GraphicsPipeline> m_AggregatePipeline;
        /// @brief Shared layout for both pipelines (set 0 bindless + set 1 point SSBO + push block).
        Ref<PipelineLayout> m_Layout;
        /// @brief Set-1 layout for the resident point SSBO (binding 0 storage buffer).
        Ref<DescriptorSetLayout> m_SetLayout;

        /// @brief The per-point sprite expansion + compaction compute pipeline (null if unsupported).
        ///
        /// Built only when the device supports the storage/indirect features the compute path needs;
        /// when null, every field takes the direct path. Reads points + runs and writes the record
        /// buffer + indirect args through m_ComputeSetLayout at set 1.
        Ref<ComputePipeline> m_ComputePipeline;
        /// @brief Layout for the expansion pipeline (set 1 compute bindings + the expansion push block).
        Ref<PipelineLayout> m_ComputeLayout;
        /// @brief Set-1 layout for the compute pass (points, runs, records, args, cursor).
        Ref<DescriptorSetLayout> m_ComputeSetLayout;

        /// @brief Whether the compute path is available (pipeline built) this pass.
        bool m_ComputeSupported = false;
        /// @brief Test/A-B hook forcing every field onto the direct path.
        bool m_ForceDirect = false;

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
        /// @brief Walk-side stats WalkFields accumulates and DrawFields finalizes into m_Stats.
        PointFieldStats m_PendingStats;

        /// @brief The g-buffer depth bindless slot for the sprite fragment's occluded fade.
        u32 m_DepthTextureIndex = 0;
        /// @brief The shared sampler bindless slot for the depth sample.
        u32 m_SamplerIndex = 0;
    };
}
