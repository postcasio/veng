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

        /// @brief Maximum screen-space size in pixels of an aggregate cell's splat.
        ///
        /// A density cell's splat covers the cell's projected screen footprint, clamped to
        /// [MinPixels, this]; its brightness spreads the cell's summed flux over that area, so
        /// the splat delivers the same integrated light the cell's resolved sprites would.
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

        /// @brief Builds a GPU-resident point field from a point set.
        ///
        /// Uploads the points into a device buffer and buckets them into a uniform cull grid.
        /// @param context  The render context the GPU resources are created on.
        /// @param info      The point set, cell size, and name.
        /// @return The built field.
        /// @pre info.Points.size() <= MaxPoints (asserted).
        static Unique<PointField> Create(Context& context, const PointFieldInfo& info);

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

        /// @brief One spatial cull cell: a world AABB over a contiguous run of the point buffer.
        ///
        /// The build sorts points into cells, so a cell's points are the buffer range
        /// [FirstPoint, FirstPoint+PointCount). A pass frustum-tests Bounds, then draws that
        /// range as sprites or, using the precomputed aggregate summary, as one density splat.
        struct Cell
        {
            /// @brief World-space bounds of the cell's points.
            AABB Bounds;
            /// @brief World-space centroid of the cell's points (the aggregate splat's center).
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
}
