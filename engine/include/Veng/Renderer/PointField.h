#pragma once

#include <span>

#include <Veng/Veng.h>
#include <Veng/Math/AABB.h>

namespace Veng::Renderer
{
    class Context;
    class Buffer;

    /// @brief One point in a field: world position, packed color, and screen size.
    ///
    /// glm-only so it lives in a public header. A field owns a large set of these in a GPU
    /// buffer, expanded on the GPU into camera-facing sprites (the resolved LOD) or summed
    /// into a per-cell aggregate splat (the density LOD). Sixteen bytes, matching the shader's
    /// GpuFieldPoint byte-for-byte, so the whole set uploads with no per-point conversion.
    struct FieldPoint
    {
        /// @brief World-space position of the point.
        vec3 Position;
        /// @brief Color as packed RGBA8 (little-endian: R in the low byte), the sprite tint and the
        ///        aggregate's summed brightness/hue source.
        u32 ColorRgba8;
        /// @brief Sprite edge length in world units at the point's depth (0 for a point-sized speck).
        f32 Size;
    };

    /// @brief Where in the frame a point field's additive radiance accumulates.
    ///
    /// HdrTail (the default) draws the field into the final HDR color after the TAA resolve and
    /// the SSR composite, ahead of bloom — the field's sub-pixel sprites skip the temporal
    /// resolve, and its radiance still rolls off through the tone curve and blooms. SceneColor
    /// draws the field into the lit scene color instead, before the translucent pass: the field
    /// then behaves as part of the opaque scene — a translucent surface draws over it and blends
    /// against it, a refractive material's scene-color grab includes it, SSR reflects it, and TAA
    /// (when on) resolves it (sub-pixel sprites pick up the temporal filter, a sharpness trade
    /// the placement opts into).
    enum class PointFieldPlacement : u8
    {
        /// @brief Accumulate into the final HDR at the tail (post-TAA/SSR, pre-bloom). The default.
        HdrTail,
        /// @brief Accumulate into the lit scene color, ahead of the translucent pass.
        SceneColor,
    };

    /// @brief Which submission path drew a field's resolved sprites this Execute.
    ///
    /// A field's sprite path is selected per field, automatically: the compute expansion pipeline
    /// when the device supports it and the field's record ring is sized to its point count, else
    /// the direct per-run vertex-stage path (the fallback for a device without the required
    /// features, the A/B verification reference, and the first frame after a field rebuild). The
    /// point-field analogue of GetActiveCullMode()'s honest reporting: the pass reports the path
    /// that actually drew, not the one requested.
    enum class SpriteDrawSource : u8
    {
        /// @brief No resolved sprites drawn this Execute (every field aggregated or was empty).
        None,
        /// @brief The direct per-run path: the sprite vertex stage runs the per-point math per corner.
        Direct,
        /// @brief The compute expansion path: per-point work runs once, compacted, drawn indirect.
        Compute,
    };

    /// @brief Per-frame point-field draw statistics, aggregated across every field the last Execute.
    ///
    /// Refreshed by the point-field pass on every Execute and zeroed on a frame that draws no field,
    /// so a mid-frame reader never sees a partial frame. The observability surface a consumer
    /// profiling a heavy field reads instead of guessing the sprite/splat split from GPU timestamps;
    /// reached through SceneRenderer::GetPointFieldStats(). CellsMeasured is tracked separately from
    /// CellsInFrustum, and ResolvedDraws separately from SpritePoints, so the walk cost (footprint
    /// measures) and the submission cost (draw calls vs. points submitted) each read out on their own.
    struct PointFieldStats
    {
        /// @brief Fields walked this Execute (resolved component, nonzero points, opacity > 0).
        u32 Fields = 0;
        /// @brief Cells walked across those fields (every cell tested, in-frustum or not).
        u32 CellsTotal = 0;
        /// @brief Cells that survived the frustum test.
        u32 CellsInFrustum = 0;
        /// @brief Cells whose on-screen footprint was measured (the density estimate).
        ///
        /// Zero when the field's threshold pins every cell to one path (a fixed-outcome fast
        /// path skips the density measure); otherwise the in-frustum cell count.
        u32 CellsMeasured = 0;
        /// @brief Sprite draw calls issued (one per contiguous run of resolved cells, or one indirect
        ///        draw per field on the compute path).
        u32 ResolvedDraws = 0;
        /// @brief Points submitted through the sprite path (summed over the resolved cells).
        ///
        /// The pre-compaction point total: the count the direct path draws and the compute path's
        /// dispatch covers. CompactedPoints is how many of these the compute pass dropped.
        u64 SpritePoints = 0;
        /// @brief Points the compute expansion pass compacted out (behind-eye, sub-epsilon, offscreen).
        ///
        /// Zero on the direct path (it draws every submitted point). On the compute path this is
        /// SpritePoints minus the survivors the indirect draw expanded — the zero-contribution
        /// points that never cost a vertex invocation or a fragment. Read back one frame late (the
        /// GPU writes the survivor count; the CPU reads the prior frame's), so it lags a view change
        /// by a frame, exactly like GetLastGpuSurvivorCount.
        u64 CompactedPoints = 0;
        /// @brief Which path drew this Execute's resolved sprites (compute, direct, or none).
        SpriteDrawSource DrawSource = SpriteDrawSource::None;
        /// @brief Aggregate splat records drawn (one per aggregated cell).
        u32 Splats = 0;
    };

    /// @brief Screen-density LOD and photometric knobs for a PointField draw.
    ///
    /// The pass estimates each visible cell's on-screen point density (points per pixel of the
    /// cell's projected footprint) and switches the cell between the two draw paths on this
    /// threshold, with a hysteresis band so a cell hovering at the boundary does not flip every
    /// frame. The threshold is a knob, not a policy: the pass reports and switches on the bound
    /// the consumer configures, it does not decide when aggregation is desirable.
    ///
    /// Both paths render a point as a constant-surface-brightness disc whose emitted flux is
    /// proportional to Color * Size^2, so a point's apparent brightness falls off with the square
    /// of its distance and the two paths agree on total light: a sprite whose projected size is
    /// clamped conserves its flux by scaling brightness, and a cell's aggregate splat spreads the
    /// cell's summed flux over its projected footprint.
    struct PointFieldLod
    {
        /// @brief How an aggregated cell's density splat is sized and normalized across the cull cell.
        ///
        /// Selects the photometric model the aggregate LOD draws with, trading sharp isolated cores
        /// against a seamless extended field. It changes only the aggregate path; the resolved
        /// sprite path is identical for both styles.
        enum class AggregateStyle : u8
        {
            /// @brief A collapsing point cloud: occupancy-adaptive splats, distance-dimmed.
            ///
            /// A cell's splat footprint follows its occupancy — the points' projected bounds for a
            /// sparse cell (an isolated point stays a sharp dot at its own position), the projected
            /// cell edge for a filled one — and its flux is conserved across the size clamp, so a
            /// receding cell dims with the square of distance like the real emitters it stands in
            /// for. The default; suits a field of resolvable points collapsing with distance.
            Cloud,

            /// @brief A seamless extended field: lattice-tiling splats, surface-brightness-preserving.
            ///
            /// A cell's splat widens to the full cell as its centroid centers within its grid cell
            /// (so a dense run's splats tile to a partition of unity with no cull-grid imprint) and
            /// tightens to the points' own bounds as the centroid drifts off-center (a sparse
            /// cluster off the lattice). Its per-pixel color is normalized by that footprint rather
            /// than a pixel floor, holding the field's surface brightness constant as the camera
            /// recedes — an extended source does not dim per pixel with distance, so a receding cell
            /// shrinks and tiles rather than fading out. Suits a smooth unresolved backdrop glow.
            Continuous,
        };

        /// @brief The aggregate splat's photometric style (see AggregateStyle).
        AggregateStyle Style = AggregateStyle::Cloud;

        /// @brief Whether sprites test the scene depth and fade when occluded (default on).
        ///
        /// Off skips the per-fragment depth sample entirely — right for a field composited over
        /// background with no occluding geometry, where the fade can never trigger.
        bool DepthFade = true;

        /// @brief Points-per-pixel above which a cell draws as the aggregate density splat.
        ///
        /// A cell whose visible points, divided by its projected screen footprint in pixels,
        /// exceed this draw as one additive splat rather than individual sprites. Larger keeps
        /// cells resolving into sprites longer (more sprites drawn); smaller collapses to the
        /// aggregate sooner (cheaper when the whole field is in view).
        f32 AggregateThreshold = 0.25f;

        /// @brief Fractional hysteresis half-width around the threshold.
        ///
        /// A cell already aggregating stays aggregated until its density falls below
        /// AggregateThreshold*(1-Hysteresis); a cell resolving stays resolved until its density
        /// rises above AggregateThreshold*(1+Hysteresis). The dead band between the two damps the
        /// per-cell sprite<->aggregate transition so it does not pop on a slow zoom.
        f32 Hysteresis = 0.2f;

        /// @brief Maximum drawn screen-space footprint in pixels of an aggregate cell's splat.
        ///
        /// A density cell's splat footprint follows its occupancy — the points' projected
        /// bounds for a sparse cell, the projected cell edge for a filled one — capped at this
        /// (the splat's soft kernel extends past the footprint to overlap its neighbors,
        /// carrying no extra net light); its brightness spreads the cell's summed flux over
        /// the footprint area, so the splat delivers the same integrated light the cell's
        /// resolved sprites would.
        f32 AggregateSplatPixels = 96.0f;

        /// @brief Overall brightness scale applied to the aggregate splat.
        ///
        /// Applied on top of the flux-conserving normalization, so a consumer biases how bright
        /// a collapsed region glows relative to its resolved sprites without re-sizing the splat.
        f32 AggregateIntensity = 1.0f;

        /// @brief Smallest drawn sprite size in pixels.
        ///
        /// A sprite whose projected size falls below this draws at this size with its brightness
        /// scaled by (projected/drawn)^2 — flux-conserving, so a receding point dims like a real
        /// emitter instead of shrinking below a pixel and shimmering out.
        f32 MinPixels = 1.25f;

        /// @brief Largest drawn sprite size in pixels.
        ///
        /// A sprite whose projected size exceeds this draws at this size with its brightness
        /// scaled up by the same flux-conserving square, capped by MaxIntensity — a very near or
        /// very large emitter reads as a small, HDR-bright core (bloom supplies the glow) rather
        /// than a huge dull disc.
        f32 MaxPixels = 8.0f;

        /// @brief Cap on any single sprite's or splat's peak drawn brightness.
        ///
        /// Bounds the (projected/drawn)^2 gain applied when MaxPixels clamps a sprite down, and
        /// the aggregate splat's flux-normalized per-pixel color, so no point or cell can push
        /// unbounded HDR values into the accumulation target — a dense cell saturates to the same
        /// ceiling on either LOD path.
        f32 MaxIntensity = 32.0f;

        /// @brief Uniform multiplier on this field's emitted radiance, for both LOD paths.
        ///
        /// Scales the sprite and aggregate output color by a constant, so a consumer fades a field's
        /// whole contribution in or out — down to invisible at 0 — without rebuilding it or touching
        /// the resident points. Applied on top of every other photometric knob; 1 is unchanged.
        f32 Opacity = 1.0f;

        /// @brief Diffraction-spike strength on resolved sprites; 0 (the default) is the plain disc.
        ///
        /// Above zero the sprite kernel adds four axis-aligned diffraction spikes of this relative
        /// strength over the soft disc core — the bright-star look. The kernel renormalizes so a
        /// sprite's integrated flux matches the plain disc at any strength, keeping the
        /// sprite<->aggregate LOD transition brightness-stable. Spikes need drawn pixels to read:
        /// pair a non-zero strength with a larger MaxPixels. Resolved sprites only — an aggregate
        /// splat is an unresolved glow and never draws spikes.
        f32 SpriteSpikes = 0.0f;

        /// @brief Per-cell aggregate anchor jitter, as a fraction of the cull cell size (Cloud only).
        ///
        /// A filled cell's point centroid converges on its cull-cell center, so its aggregate splat
        /// sits on the cull lattice; where such splats are drawn narrower than their cell, coverage
        /// dips along the shared cell boundaries, and the boundary planes seen edge-on read as a
        /// grid imprint on an otherwise smooth field. Offsetting each cell's splat anchor by a
        /// deterministic hash of its lattice coordinates — up to this fraction of the cell size on
        /// each axis — knocks the splats off the lattice so those coverage dips scatter into faint
        /// grain instead of a coherent grid. The jitter is stable frame to frame and rebuild to
        /// rebuild (it is a pure function of the cell's coordinates). Applies to the Cloud style
        /// only — a Continuous cell already widens to tile the full cell — and 0 disables it.
        f32 AnchorJitter = 0.0f;
    };

    /// @brief Construction parameters for a PointField.
    struct PointFieldInfo
    {
        /// @brief Debug name for the field's GPU resources.
        string Name = "PointField";
        /// @brief The points that make up the field (uploaded once at build).
        ///
        /// The count must not exceed PointField::MaxPoints; a larger set is a consumer error
        /// asserted at build (the whole field is GPU-resident, with no streaming or eviction —
        /// a variable consumer caps its realized count to MaxPoints).
        std::span<const FieldPoint> Points;
        /// @brief Edge length in world units of one spatial cull cell.
        ///
        /// Points are bucketed into a uniform grid of this cell size so a view culls whole cells
        /// cheaply rather than testing every point. Smaller cells cull tighter but cost more
        /// per-cell metadata; a cell size near the mean inter-point spacing balances the two.
        f32 CellSize = 8.0f;
    };

    struct PointFieldBuildInfo;

    /// @brief A large, GPU-resident set of positioned, colored, sized points with a distance LOD.
    ///
    /// Built once from a point set and drawn many frames by a PointFieldScenePass. A consumer
    /// assigns the built field to a scene PointField component's Field; the SceneRenderer walks
    /// those components each Execute and draws every live field. The pass frustum-culls the field's
    /// spatial cells against the camera, then per visible cell
    /// draws either individual camera-facing sprites (the resolved LOD) or a single additive
    /// density splat (the aggregate LOD) selected by the on-screen point density — so the cost
    /// stays bounded when the whole field is in view.
    ///
    /// The field is persistent, not immediate-mode: unlike the DebugDraw accumulator it is not
    /// re-uploaded per frame. Updates are explicit — a sub-range Write, or a full rebuild by
    /// constructing a new field. Single-owner (Unique); Create is the factory. Points are an
    /// unlit emissive primitive, drawn by their own lightweight pass, not a Material/g-buffer path.
    class PointField
    {
    public:
        /// @brief Maximum resident point count.
        ///
        /// One FieldPoint is 16 bytes, so the whole field caps the resident point buffer at
        /// MaxPoints * 16 B ≈ 64 MiB — the validated VRAM envelope. A consumer with a variable
        /// set caps its realized count to this so the field never outgrows what was validated;
        /// a larger set is a consumer error asserted at build.
        static constexpr u32 MaxPoints = 4u * 1024u * 1024u;

        /// @brief One spatial cull cell: a world AABB over a contiguous run of the point buffer.
        ///
        /// The build sorts points into cells, so a cell's points are the buffer range
        /// [FirstPoint, FirstPoint+PointCount). A pass frustum-tests Bounds, then draws that
        /// range as sprites or, using the precomputed aggregate summary, as one density splat.
        struct Cell
        {
            /// @brief World-space bounds of the cell's points.
            AABB Bounds;
            /// @brief World-space centroid of the cell's points (the aggregate splat's anchor).
            vec3 Centroid;
            /// @brief Sum of the cell's point fluxes (linear RGB color * Size^2), the splat's light source.
            ///
            /// Summed at build so the aggregate draw never re-reads the point buffer per frame; the
            /// pass spreads it over the cell's projected footprint for the additive splat, matching
            /// the integrated light of the cell's resolved sprites. A zero-Size point contributes
            /// no flux, so a field of point-sized specks has no aggregate representation.
            vec3 SummedFlux;
            /// @brief Index of the cell's first point in the resident buffer.
            u32 FirstPoint;
            /// @brief Number of points in the cell.
            u32 PointCount;
        };

        /// @brief A prebucketed field build: the cell-sorted points, their cull cells, and bounds.
        ///
        /// The product of Bucket — the CPU half of a field build. Points holds the set rewritten
        /// in cell-sorted order (each Cell indexes a contiguous run of it), so building a field
        /// from this is upload-only.
        struct BuildData
        {
            /// @brief The point set in cell-sorted order (the order Cells index into).
            vector<FieldPoint> Points;
            /// @brief The spatial cull cells over Points.
            vector<Cell> Cells;
            /// @brief World-space bounds over every point.
            AABB Bounds = AABB::Empty();
        };

        /// @brief Buckets a point set into the cell-sorted build a field uploads — device-free.
        ///
        /// The CPU half of a field build: groups the points into a uniform grid of cellSize
        /// cells, rewrites them so each cell's points are contiguous, and precomputes each cell's
        /// bounds, centroid, and summed flux. Touches no GPU resource or Context, so a consumer
        /// may run it on a TaskSystem worker and hand the result to the prebucketed Create on the
        /// render thread.
        /// @param points    The points to bucket.
        /// @param cellSize  Edge length in world units of one cull cell.
        /// @return The cell-sorted build.
        /// @pre cellSize > 0 (asserted).
        [[nodiscard]] static BuildData Bucket(std::span<const FieldPoint> points, f32 cellSize);

        /// @brief Builds a GPU-resident point field from a point set.
        ///
        /// Buckets the points into a uniform cull grid (Bucket, inline on the calling thread) and
        /// uploads the cell-sorted set into a device buffer — the one-call convenience over the
        /// Bucket + prebucketed-Create split.
        /// @param context  The render context the GPU resources are created on.
        /// @param info      The point set, cell size, and name.
        /// @return The built field.
        /// @pre info.Points.size() <= MaxPoints (asserted).
        static Unique<PointField> Create(Context& context, const PointFieldInfo& info);

        /// @brief Builds a GPU-resident point field from a prebucketed build (see Bucket).
        ///
        /// The upload half of the split build: creates the resident buffer and uploads the
        /// already-sorted points, adopting the build's cells and bounds. The CPU bucketing
        /// happened in Bucket, so this is the whole render-thread cost of a field produced on a
        /// worker.
        /// @param context  The render context the GPU resources are created on.
        /// @param info      The prebucketed build, cell size, and name.
        /// @return The built field.
        /// @pre info.Data.Points.size() <= MaxPoints (asserted).
        static Unique<PointField> Create(Context& context, PointFieldBuildInfo info);

        /// @brief Destroys the field's GPU resources.
        ~PointField();

        PointField(const PointField&) = delete;
        PointField& operator=(const PointField&) = delete;

        /// @brief Overwrites a contiguous sub-range of the resident points.
        ///
        /// A partial update path: the caller rewrites [firstPoint, firstPoint+points.size()) of
        /// the resident buffer without rebuilding the whole field. The cull grid is rebucketed to
        /// reflect the new positions, so a moved point culls correctly.
        /// @param firstPoint  Index of the first point to overwrite.
        /// @param points      The replacement points.
        /// @pre firstPoint + points.size() <= GetPointCount() (asserted).
        void Write(u32 firstPoint, std::span<const FieldPoint> points);

        /// @brief Sets this field's screen-density LOD knobs.
        void SetLod(const PointFieldLod& lod) { m_Lod = lod; }

        /// @brief Returns this field's screen-density LOD knobs.
        [[nodiscard]] const PointFieldLod& GetLod() const { return m_Lod; }

        /// @brief Returns the number of resident points.
        [[nodiscard]] u32 GetPointCount() const { return m_PointCount; }

        /// @brief Returns the field's world-space bounding box (the union of every point).
        [[nodiscard]] const AABB& GetBounds() const { return m_Bounds; }

        /// @brief Returns the cull cell edge length in world units.
        [[nodiscard]] f32 GetCellSize() const { return m_CellSize; }

        /// @brief Returns the spatial cull cells, each a bounded contiguous point range.
        [[nodiscard]] const vector<Cell>& GetCells() const { return m_Cells; }

        /// @brief Returns the resident point buffer (set 1 binding 0 for the draw), for the pass.
        [[nodiscard]] const Ref<Buffer>& GetPointBuffer() const { return m_PointBuffer; }

    private:
        PointField() = default;

        /// @brief Buckets the resident points into the uniform cull grid, refreshing m_Cells/m_Bounds.
        void Rebucket(std::span<const FieldPoint> points);

        /// @brief The render context (for the sub-range upload).
        Context* m_Context = nullptr;
        /// @brief The resident point set (device-local, sub-range updatable).
        Ref<Buffer> m_PointBuffer;
        /// @brief Number of resident points.
        u32 m_PointCount = 0;
        /// @brief Cull cell edge length in world units.
        f32 m_CellSize = 8.0f;
        /// @brief World-space bounds over every point.
        AABB m_Bounds = AABB::Empty();
        /// @brief The spatial cull cells (bounded contiguous point ranges).
        vector<Cell> m_Cells;
        /// @brief This field's screen-density LOD knobs.
        PointFieldLod m_Lod;
    };

    /// @brief Construction parameters for a PointField from a prebucketed build.
    ///
    /// See PointField::Bucket for producing Data; CellSize is the size the bucketing ran at.
    struct PointFieldBuildInfo
    {
        /// @brief Debug name for the field's GPU resources.
        string Name = "PointField";
        /// @brief Edge length in world units of one spatial cull cell (the size Data was bucketed at).
        f32 CellSize = 8.0f;
        /// @brief The prebucketed build to adopt: cell-sorted points, cull cells, and bounds.
        PointField::BuildData Data;
    };
}
