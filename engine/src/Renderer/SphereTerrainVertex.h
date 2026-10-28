#pragma once

#include <array>

#include <Veng/Veng.h>
#include <Veng/Renderer/SphereTerrain.h>

namespace Veng::Renderer
{
    /// @brief The vertices of the shared terrain grid's surface: `(G + 1)²`.
    inline constexpr u32 SphereTerrainSurfaceVertices =
        (SphereTerrainGridQuads + 1) * (SphereTerrainGridQuads + 1);

    /// @brief The vertices of the grid's skirt: one under every edge vertex, `4 (G + 1)`.
    inline constexpr u32 SphereTerrainSkirtVertices = 4 * (SphereTerrainGridQuads + 1);

    /// @brief The triangles a tile draws: two a quad, plus two a skirt quad along each edge.
    inline constexpr u32 SphereTerrainTileTriangles =
        (2 * SphereTerrainGridQuads * SphereTerrainGridQuads) + (8 * SphereTerrainGridQuads);

    /// @brief The grid's index count.
    inline constexpr u32 SphereTerrainGridIndexCount = 3 * SphereTerrainTileTriangles;

    /// @brief The texel step between two grid vertices.
    inline constexpr u32 SphereTerrainGridTexelStep =
        SphereTerrainTileCells / SphereTerrainGridQuads;

    /// @brief How long a split's children take to morph in from their parent's shape when a
    ///        split was delayed past its distance, in seconds.
    inline constexpr f32 SphereTerrainArrivalSeconds = 0.25f;

    /// @brief The shortest skirt a tile hangs, in metres.
    inline constexpr f32 SphereTerrainMinSkirtMetres = 0.05f;

    /// @brief The record flag set when the tile has a parent to morph toward.
    inline constexpr u32 SphereTerrainRecordHasParent = 1u << 0;

    /// @brief The record flag set when the selection held the tile whole
    ///        (SphereTerrainDrawnTile::Held).
    inline constexpr u32 SphereTerrainRecordHeld = 1u << 1;

    /// @brief The record flag set when the view draws the tile overlay
    ///        (SceneView::SphereTerrainTiles).
    inline constexpr u32 SphereTerrainRecordTileOverlay = 1u << 2;

    /// @brief One drawn tile's record, read by the terrain vertex and fragment stages; mirrors
    ///        `SphereTerrainRecord` in `Veng/sphere_terrain.slang` byte for byte (std430).
    ///
    /// Positions are eye-relative and in scene axes: each corner is
    /// `Center + Orientation · (Radius · d_i) − eye`, formed in double on the host and rounded to
    /// f32, so nothing the size of the radius is ever subtracted on the device.
    struct SphereTerrainDrawRecord
    {
        /// @brief The tile's body-fixed placement: its centre direction, corner offsets, norms and
        ///        warp terms (`Veng/sphere_terrain_mapping.slang`).
        SphereTerrainTileConstants Tile;
        /// @brief Per corner (index sx + 2 sy), its eye-relative position in metres (xyz).
        std::array<vec4, 4> Corners{};
        /// @brief Per corner, its unit vertical in scene axes (xyz).
        std::array<vec4, 4> Verticals{};
        /// @brief The squared chords between corners: |d0−d1|², |d0−d2|², |d0−d3|², |d1−d2|².
        vec4 ChordsA{0.0f};
        /// @brief |d1−d3|², |d2−d3|², the radius in metres, and the scale about the eye.
        vec4 ChordsB{0.0f};
        /// @brief Per corner, last frame's position in last frame's scene space, relative to this
        ///        frame's camera position (xyz).
        std::array<vec4, 4> PrevCorners{};
        /// @brief The tile's tangent along its `u` axis in scene axes (xyz), unit.
        vec4 TangentU{0.0f};
        /// @brief The morph start and end distances, the arrival floor, and the skirt depth.
        vec4 Morph{0.0f};
        /// @brief The level layers' heights above the radius (xy), the texel size in metres (z).
        vec4 Heights{0.0f};
        /// @brief The tile's layer, its parent's layer, its quadrant in the parent, and the
        ///        SphereTerrainRecord* flags.
        uvec4 Layers{0u};
        /// @brief The generator's words for the tile.
        std::array<u32, MaxSphereTerrainTileWords> Data{};
        /// @brief The generator's words for the tile's parent.
        std::array<u32, MaxSphereTerrainTileWords> ParentData{};
    };
    static_assert(sizeof(SphereTerrainDrawRecord) == 672,
                  "SphereTerrainDrawRecord must match the shader's std430 layout");

    /// @brief A grid vertex: its coordinate on the grid, and whether it is a skirt vertex hung
    ///        under that edge vertex.
    struct SphereTerrainGridVertex
    {
        /// @brief The column and row, 0 … SphereTerrainGridQuads.
        ivec2 Grid{0};
        /// @brief Whether the vertex hangs under its edge vertex by the tile's skirt depth.
        bool Skirt = false;
    };

    /// @brief Decodes a grid vertex index: the surface's `(G + 1)²` row-major, then one skirt
    ///        vertex per edge vertex, edge by edge in Edge order.
    [[nodiscard]] VE_API SphereTerrainGridVertex DecodeSphereTerrainGridVertex(u32 index);

    /// @brief The shared grid's index list.
    ///
    /// Every quad is cut on the diagonal from its lowest corner to its highest, which is the one
    /// whose collapse under the morph reproduces the parent's triangulation: odd coordinates slide
    /// to the even coordinate below them, so the child quad whose lowest corner is odd on both axes
    /// becomes its parent quad, cut the same way, and every other quad collapses to a line or a
    /// point. Triangles wind counter-clockwise seen from outside the sphere; a skirt's from
    /// outside its tile.
    [[nodiscard]] VE_API std::array<u16, SphereTerrainGridIndexCount>
    BuildSphereTerrainGridIndices();

    /// @brief A grid coordinate slid by the morph: each odd coordinate toward the even one below
    ///        it by @p morph, so at one every vertex stands on a parent grid vertex.
    [[nodiscard]] VE_API vec2 MorphSphereTerrainGrid(ivec2 grid, f32 morph);

    /// @brief A tile grid coordinate on its parent's grid.
    /// @param grid      The coordinate on the tile's grid.
    /// @param quadrant  The tile's quadrant in its parent (CubeSphereTileKey::GetQuadrant).
    [[nodiscard]] VE_API vec2 SphereTerrainParentGrid(vec2 grid, u32 quadrant);

    /// @brief A height over a grid, interpolated linearly over the grid's triangles at a
    ///        fractional grid coordinate.
    /// @tparam Height  `f32(ivec2 vertex)`: the height at a grid vertex.
    template <typename Height>
    [[nodiscard]] f32 SphereTerrainGridHeight(const vec2 grid, const Height& height)
    {
        const f32 quads = static_cast<f32>(SphereTerrainGridQuads);
        const vec2 cell = glm::clamp(glm::floor(grid), vec2(0.0f), vec2(quads - 1.0f));
        const vec2 f = grid - cell;
        const ivec2 c(cell);
        const f32 h00 = height(c);
        const f32 h11 = height(c + ivec2(1, 1));
        if (f.x >= f.y)
        {
            const f32 h10 = height(c + ivec2(1, 0));
            return h00 + (f.x * (h10 - h00)) + (f.y * (h11 - h10));
        }
        const f32 h01 = height(c + ivec2(0, 1));
        return h00 + (f.y * (h01 - h00)) + (f.x * (h11 - h01));
    }

    /// @brief The morph factor at a vertex: `max(saturate((d − S) / (E − S)), floor)`.
    [[nodiscard]] VE_API f32 SphereTerrainMorphFactor(f32 distance, f32 start, f32 end, f32 floor);

    /// @brief A placed vertex.
    struct SphereTerrainPlacement
    {
        /// @brief Its eye-relative position in scene axes, in metres.
        vec3 Position{0.0f};
        /// @brief The sphere's unit vertical there, in scene axes.
        vec3 Vertical{0.0f};
    };

    /// @brief Places a vertex of a tile at tile parameters and a height: the f32 twin of the
    ///        terrain stages' `SphereTerrainPlace`.
    ///
    /// With corner weights `ŵ_i` of the warped fractions, the chord point's shortfall from the
    /// sphere is `1 − |m|² = Σ_{i<j} ŵ_i ŵ_j |d_i − d_j|²`, from the record's exact chords, and the
    /// position is `Σ ŵ_i C_i + n · (R (1 − |m|²) / (1 + |m|) + h)` with `n` the normalised
    /// weighted verticals. Every term is an eye-relative corner, rounded in proportion to its own
    /// distance from the eye, or a quantity no larger than the tile, so a vertex under the eye on
    /// a sphere thousands of kilometres across is placed to well under a millimetre.
    /// @param record  The tile's record.
    /// @param st      The tile parameters; 0 and 1 are the tile's edges.
    /// @param height  The height above the radius, in metres.
    [[nodiscard]] VE_API SphereTerrainPlacement
    PlaceSphereTerrainVertex(const SphereTerrainDrawRecord& record, vec2 st, f32 height);

    /// @brief One evaluated vertex: the f32 twin of the terrain stages' `SphereTerrainEvaluate`.
    struct SphereTerrainEvaluated
    {
        /// @brief The placed vertex, morphed and with its skirt hung.
        SphereTerrainPlacement Placement;
        /// @brief The morph factor it took.
        f32 Morph = 0.0f;
        /// @brief The morphed tile parameters.
        vec2 St{0.0f};
    };

    /// @brief Evaluates a grid vertex of a tile as the terrain stages do: placed unmorphed to
    ///        measure its eye distance, morphed (odd coordinates slid toward the even one below,
    ///        the height blended from the tile's own grid to its parent's), placed again, and hung
    ///        by the skirt depth when it is a skirt vertex.
    /// @tparam OwnHeight     `f32(ivec2)`: the tile's height at one of its grid vertices.
    /// @tparam ParentHeight  `f32(ivec2)`: the parent's height at one of its grid vertices.
    template <typename OwnHeight, typename ParentHeight>
    [[nodiscard]] SphereTerrainEvaluated
    EvaluateSphereTerrainVertex(const SphereTerrainDrawRecord& record,
                                const SphereTerrainGridVertex vertex, const OwnHeight& own,
                                const ParentHeight& parent)
    {
        const f32 quads = static_cast<f32>(SphereTerrainGridQuads);
        const f32 gridHeight = own(vertex.Grid);
        const SphereTerrainPlacement unmorphed =
            PlaceSphereTerrainVertex(record, vec2(vertex.Grid) / quads, gridHeight);
        const f32 k = SphereTerrainMorphFactor(glm::length(unmorphed.Position), record.Morph.x,
                                               record.Morph.y, record.Morph.z);
        const ivec2 odd = vertex.Grid & ivec2(1);
        const vec2 slid = MorphSphereTerrainGrid(vertex.Grid, k);
        const vec2 parentGrid = SphereTerrainParentGrid(slid, record.Layers.z);
        const f32 ownHeight = glm::mix(gridHeight, own(vertex.Grid - odd), k);
        const f32 height = glm::mix(ownHeight, SphereTerrainGridHeight(parentGrid, parent), k);
        SphereTerrainEvaluated result{.Morph = k, .St = slid / quads};
        result.Placement = PlaceSphereTerrainVertex(record, result.St, height);
        if (vertex.Skirt)
        {
            result.Placement.Position -= result.Placement.Vertical * record.Morph.w;
        }
        return result;
    }

    /// @brief The corner weights `ŵ_i` at tile parameters.
    [[nodiscard]] VE_API vec4 SphereTerrainCornerWeights(const SphereTerrainTileConstants& tile,
                                                         vec2 st);
}
