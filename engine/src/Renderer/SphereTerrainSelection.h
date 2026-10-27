#pragma once

#include <array>
#include <unordered_set>

#include <Veng/Veng.h>
#include <Veng/Math/CubeSphere.h>
#include <Veng/Renderer/SphereTerrain.h>

#include "SphereTerrainResidency.h"

namespace Veng::Renderer
{
    /// @brief A tile's bound: a box in the tile's own frame (its centre's tangent plane and
    ///        normal), over its corners, edge midpoints and centre at both ends of its working
    ///        height range, grown by the sphere's sagitta between those samples.
    ///
    /// Oriented to the tile, so its diameter is the tile's own whatever face the tile is on.
    struct SphereTerrainTileBox
    {
        /// @brief The frame's origin, on the sphere below the tile's centre, body-fixed metres.
        dvec3 Origin{0.0};
        /// @brief The frame's axes: across the tile along u, across it along v, and outward.
        std::array<dvec3, 3> Axes{};
        /// @brief The box's lower corner in the frame.
        dvec3 Min{0.0};
        /// @brief The box's upper corner in the frame.
        dvec3 Max{0.0};
    };

    /// @brief A level's largest vertex spacing, in metres: an upper bound on any tile's longest
    ///        edge arc at the level over the grid's quads, halving exactly per level.
    [[nodiscard]] VE_API f64 SphereTerrainLevelSpacing(f64 radius, u32 level);

    /// @brief A level's largest planar tile diameter, in metres: an upper bound on any tile's
    ///        corner-to-corner extent at the level, halving exactly per level.
    [[nodiscard]] VE_API f64 SphereTerrainLevelDiameter(f64 radius, u32 level);

    /// @brief A level's range: the distance from which a tile of the level is adequate,
    ///        `max(s_L · K / τ, c · Ď_L)`. Exactly twice the next level's.
    /// @param radius              The sphere's radius.
    /// @param pixelsPerRadian     K: half the view's height in pixels over `tan(fovY / 2)`.
    /// @param targetPixelsPerQuad τ.
    /// @param planarRangeFloor    c.
    /// @param level               The level.
    [[nodiscard]] VE_API f64 SphereTerrainLevelRange(f64 radius, f64 pixelsPerRadian,
                                                     f32 targetPixelsPerQuad, f32 planarRangeFloor,
                                                     u32 level);

    /// @brief Builds a tile's bound box over a height range.
    [[nodiscard]] VE_API SphereTerrainTileBox ComputeSphereTerrainTileBox(CubeSphereTileKey key,
                                                                          f64 radius, f32 minHeight,
                                                                          f32 maxHeight);

    /// @brief The distance from a point to a tile box; zero inside it.
    [[nodiscard]] VE_API f64 SphereTerrainBoxDistance(const SphereTerrainTileBox& box,
                                                      const dvec3& point);

    /// @brief A box's eight corners in body-fixed metres.
    [[nodiscard]] VE_API std::array<dvec3, 8>
    SphereTerrainBoxCorners(const SphereTerrainTileBox& box);

    /// @brief Whether a point is hidden from the eye by a sphere about the origin: the segment
    ///        between them meets the ball. Never true for an eye inside the ball.
    [[nodiscard]] VE_API bool IsHiddenBySphere(const dvec3& eye, const dvec3& point,
                                               f64 occluderRadius);

    /// @brief Whether a whole box is hidden by the occluder sphere: all eight of its corners are.
    ///
    /// The hidden region (the ball and the cone behind its tangent circle) is convex, so eight
    /// hidden corners hide the box; the test is conservative.
    [[nodiscard]] VE_API bool IsBoxBelowHorizon(const SphereTerrainTileBox& box, const dvec3& eye,
                                                f64 occluderRadius);

    /// @brief What a renderer's selection of one sphere terrain reads.
    struct SphereTerrainSelectionInput
    {
        /// @brief The sphere's radius in metres.
        f64 Radius = 1.0;
        /// @brief The lowest height anywhere on the sphere.
        f32 MinHeight = 0.0f;
        /// @brief The highest height anywhere on the sphere.
        f32 MaxHeight = 0.0f;
        /// @brief The deepest level generated.
        u32 MaxLevel = CubeSphere::MaxLevel;
        /// @brief The generator's bound on how far a tile's range exceeds its parent's, per level.
        std::array<f32, CubeSphere::MaxLevel + 1> LevelHeightBounds{};
        /// @brief The eye, body-fixed, in metres from the sphere's centre.
        dvec3 Eye{0.0};
        /// @brief Half the view's height in pixels over `tan(fovY / 2)`.
        f64 PixelsPerRadian = 1000.0;
        /// @brief The pixels a quad edge may span before its tile splits.
        f32 TargetPixelsPerQuad = 8.0f;
        /// @brief The view frustum's planes in the body-fixed frame, relative to the eye: a point
        ///        `q` (from the eye) is inside when `dot(xyz, q) + w ≥ 0` for every plane.
        ///
        /// Beyond Settings.OmnidirectionalRadius a tile refines only while its bound meets these
        /// planes, and requests inside them rank ahead of those outside. A renderer passes the
        /// view's side planes alone, so the selection does not depend on where the near and far
        /// planes stand (or on a scale about the eye that moves them). Null treats every tile as
        /// inside.
        const std::array<glm::dvec4, 6>* Frustum = nullptr;
        /// @brief The deepest pinned level: tiles at or above it are always resident, so wanting
        ///        them costs the budget nothing.
        u32 PinnedLevel = 0;
        /// @brief The most tiles below the pinned top the selection may want kept or generated.
        ///
        /// Splits are taken nearest the eye first (by the distance to the tile's bound, a split
        /// taken last frame ranked as if `(1 − h)` of its distance away and a new one `(1 + h)`),
        /// so when the pool cannot hold every wanted split the ground under and before the eye
        /// keeps its depth and the shortfall falls on the far field. ~0 is no budget.
        u32 TileBudget = ~0u;
        /// @brief What the pool holds.
        const SphereTerrainTileOracle* Tiles = nullptr;
        /// @brief The tiles split last frame, for hysteresis.
        const std::unordered_set<u64>* PreviousSplits = nullptr;
        /// @brief The rule's tunables.
        SphereTerrainSelectionSettings Settings;
    };

    /// @brief Why a tile is requested.
    enum class SphereTerrainRequestKind : u8
    {
        /// @brief A drawn tile wants to split and this child is missing.
        Split,
        /// @brief A drawn tile is near enough that its children are worth having early.
        Prefetch,
    };

    /// @brief A tile the selection wants generated.
    struct SphereTerrainSelectionRequest
    {
        /// @brief The tile.
        CubeSphereTileKey Key;
        /// @brief Why.
        SphereTerrainRequestKind Kind = SphereTerrainRequestKind::Split;
        /// @brief How far the parent's error exceeds the threshold (≥ 1 for a split).
        f32 Urgency = 0.0f;
        /// @brief Whether the parent's bound meets the view frustum.
        bool InFrustum = true;
    };

    /// @brief A selection's result.
    struct SphereTerrainSelectionResult
    {
        /// @brief The tiles to draw: a 2:1-balanced set covering the sphere once.
        vector<SphereTerrainDrawnTile> Drawn;
        /// @brief Every resident tile wanted this frame — drawn, an ancestor of a drawn tile, a
        ///        child of a tile wanting to split, or prefetched.
        vector<CubeSphereTileKey> Wanted;
        /// @brief The tiles to request, ordered most urgent first.
        vector<SphereTerrainSelectionRequest> Requests;
        /// @brief The tiles split this frame, which the next frame's hysteresis reads.
        std::unordered_set<u64> Splits;
        /// @brief What the selection did.
        SphereTerrainSelectionStats Stats;
    };

    /// @brief Selects a sphere terrain's tiles for one view: split by screen-space error with
    ///        hysteresis, waiting on residency, 2:1 balanced, with morph ranges per drawn tile.
    ///
    /// Device-free and pure. A drawn tile at level L splits when its bound is nearer than
    /// `(1 − h)·D_L` and merges past `(1 + h)·D_L`; it also splits when its MorphError, projected at
    /// its bound's distance, exceeds the height-error target. A split happens only when all four
    /// children are resident; otherwise the tile is drawn and its children requested (the parent
    /// fallback). Subtrees are pruned below the horizon, past their level's range, and — beyond
    /// the omnidirectional radius — outside the view frustum, so a turn of the head finds the
    /// near tiles resident. Splits are taken nearest first under the input's tile budget.
    [[nodiscard]] VE_API SphereTerrainSelectionResult
    SelectSphereTerrain(const SphereTerrainSelectionInput& input);
}
