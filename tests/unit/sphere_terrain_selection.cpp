// Sphere terrain selection, device-free, over a pool that holds every tile of a flat sphere: the
// level range halves exactly per level and holds the neighbour invariant's inequality at any field
// of view; the drawn set covers the sphere once, is 2:1 balanced (also when a tile's children are
// withheld), deepens monotonically toward the ground and holds still inside the hysteresis band;
// and every edge between drawn tiles of adjacent levels has the coarse side unmorphed and the fine
// side fully morphed. Under a tile budget the ground under the eye keeps its full depth, beyond the
// near radius only tiles in the view refine, and either way the set still covers once, balances,
// and keeps the invariant wherever its coarse side was not held.

#include <algorithm>
#include <cmath>
#include <numbers>
#include <unordered_map>
#include <unordered_set>

#include <doctest/doctest.h>

#include <Veng/Math/CubeSphere.h>
#include <Veng/Math/Frustum.h>
#include <Veng/Scene/Camera.h>

#include <Renderer/SphereTerrainResidency.h>
#include <Renderer/SphereTerrainSelection.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr f64 Radius = 6.4e6;
    constexpr f64 PixelsPerRadian = 1000.0;

    // Every tile resident and measured flat, except the children of one withheld tile.
    class FlatPool final : public SphereTerrainTileOracle
    {
    public:
        const SphereTerrainTileRecord* Find(const CubeSphereTileKey key) const override
        {
            if (Withheld.IsValid() && key.GetLevel() > 0 && key.GetParent() == Withheld)
            {
                return nullptr;
            }
            m_Record.Key = key;
            return &m_Record;
        }

        CubeSphereTileKey Withheld;

    private:
        mutable SphereTerrainTileRecord m_Record{
            .State = SphereTerrainTileState::Resident,
            .Bound = {.Measured = true, .Min = 0.0f, .Max = 0.0f, .MorphError = 0.0f},
        };
    };

    SphereTerrainSelectionInput FlatInput(const FlatPool& pool, const dvec3& eye,
                                          const std::unordered_set<u64>* previous)
    {
        return {
            .Radius = Radius,
            .Eye = eye,
            .PixelsPerRadian = PixelsPerRadian,
            .Tiles = &pool,
            .PreviousSplits = previous,
        };
    }

    // Runs the selection until its splits stop changing, as a still eye's frames would.
    SphereTerrainSelectionResult Settle(const FlatPool& pool, const dvec3& eye,
                                        std::unordered_set<u64>& splits)
    {
        SphereTerrainSelectionResult result;
        for (u32 frame = 0; frame < 4; frame++)
        {
            result = SelectSphereTerrain(FlatInput(pool, eye, &splits));
            if (result.Splits == splits)
            {
                break;
            }
            splits = result.Splits;
        }
        return result;
    }

    f64 TriangleSolidAngle(const dvec3& a, const dvec3& b, const dvec3& c)
    {
        return 2.0 * std::atan2(std::abs(glm::dot(a, glm::cross(b, c))),
                                1.0 + glm::dot(a, b) + glm::dot(b, c) + glm::dot(c, a));
    }

    // Tile edges are great-circle arcs, so a tile is a spherical quadrilateral.
    f64 TileSolidAngle(const CubeSphereTileKey key)
    {
        const CubeSphereTileGeometry g = GetCubeSphereTileGeometry(key);
        return TriangleSolidAngle(g.Corners[0], g.Corners[1], g.Corners[3]) +
               TriangleSolidAngle(g.Corners[0], g.Corners[3], g.Corners[2]);
    }

    struct Adjacency
    {
        u32 Pairs = 0;
        u32 Visible = 0;
        u32 Unbalanced = 0;
        u32 CoarseMorphed = 0;
        u32 FineUnmorphed = 0;
    };

    f64 Morph(const f64 distance, const SphereTerrainDrawnTile& tile)
    {
        return std::clamp((distance - tile.MorphStart) / (tile.MorphEnd - tile.MorphStart), 0.0,
                          1.0);
    }

    // A tile's edge as tile parameters: Edge order NegU, PosU, NegV, PosV.
    glm::dvec2 EdgeParameter(const CubeSphere::Edge edge, const f64 along)
    {
        switch (edge)
        {
        case CubeSphere::Edge::NegU:
            return {0.0, along};
        case CubeSphere::Edge::PosU:
            return {1.0, along};
        case CubeSphere::Edge::NegV:
            return {along, 0.0};
        case CubeSphere::Edge::PosV:
            break;
        }
        return {along, 1.0};
    }

    // Every pair of drawn tiles meeting along an edge where the finer is finer: found from the finer
    // side, whose same-level neighbour across an edge lies inside the coarser tile. Reports the
    // level gap and the morph each side takes at the finer side's edge vertices.
    Adjacency Inspect(const vector<SphereTerrainDrawnTile>& drawn, const dvec3& eye)
    {
        std::unordered_map<u64, const SphereTerrainDrawnTile*> byKey;
        for (const SphereTerrainDrawnTile& tile : drawn)
        {
            byKey[tile.Key.Value] = &tile;
        }
        Adjacency result;
        for (const SphereTerrainDrawnTile& fine : drawn)
        {
            const CubeSphereTileGeometry geometry = GetCubeSphereTileGeometry(fine.Key);
            for (u32 e = 0; e < 4; e++)
            {
                const auto edge = static_cast<CubeSphere::Edge>(e);
                CubeSphereTileKey coarse = fine.Key.GetNeighbour(edge);
                if (byKey.contains(coarse.Value))
                {
                    continue;
                }
                u32 gap = 0;
                while (coarse.GetLevel() > 0 && !byKey.contains(coarse.Value))
                {
                    coarse = coarse.GetParent();
                    gap++;
                }
                if (gap == 0 || !byKey.contains(coarse.Value))
                {
                    continue;
                }
                result.Pairs++;
                if (gap > 1)
                {
                    result.Unbalanced++;
                    continue;
                }
                // Past the horizon the distance rule does not refine, and a split made only for
                // balance there owes no morph agreement: nothing the eye can see stands on it.
                if (IsBoxBelowHorizon(ComputeSphereTerrainTileBox(coarse, Radius, 0.0f, 0.0f), eye,
                                      Radius))
                {
                    continue;
                }
                result.Visible++;
                const SphereTerrainDrawnTile& outer = *byKey.at(coarse.Value);
                for (u32 k = 0; k <= SphereTerrainGridQuads; k++)
                {
                    const glm::dvec2 st =
                        EdgeParameter(edge, static_cast<f64>(k) / SphereTerrainGridQuads);
                    const dvec3 vertex = CubeSphereTileDirection(geometry, st.x, st.y) * Radius;
                    const f64 distance = glm::length(vertex - eye);
                    result.CoarseMorphed += Morph(distance, outer) > 0.0 ? 1u : 0u;
                    result.FineUnmorphed += Morph(distance, fine) < 1.0 ? 1u : 0u;
                }
            }
        }
        return result;
    }

    u32 DeepestLevel(const vector<SphereTerrainDrawnTile>& drawn)
    {
        u32 deepest = 0;
        for (const SphereTerrainDrawnTile& tile : drawn)
        {
            deepest = std::max(deepest, tile.Key.GetLevel());
        }
        return deepest;
    }

    // A point above the sphere, off every face's centre and edges.
    dvec3 Above(const f64 altitude)
    {
        return glm::normalize(dvec3(0.83, 0.31, -0.47)) * (Radius + altitude);
    }
}

TEST_CASE("sphere terrain selection: a level's range halves exactly and holds the invariant")
{
    const SphereTerrainSelectionSettings settings;
    const f64 h = settings.Hysteresis;
    const f64 a = settings.MorphStartRatio;
    u32 notHalved = 0;
    u32 violations = 0;
    for (const f64 k : {100.0, 1000.0, 10000.0})
    {
        for (u32 level = 0; level <= CubeSphere::MaxLevel; level++)
        {
            const f64 range =
                SphereTerrainLevelRange(Radius, k, 8.0f, settings.PlanarRangeFloor, level);
            if (level > 0)
            {
                notHalved += SphereTerrainLevelRange(Radius, k, 8.0f, settings.PlanarRangeFloor,
                                                     level - 1) == 2.0 * range
                                 ? 0u
                                 : 1u;
            }
            const f64 lhs = (1.0 + h) + SphereTerrainLevelDiameter(Radius, level) / range;
            violations += lhs <= 2.0 * a * (1.0 - h) ? 0u : 1u;
        }
    }
    CHECK(notHalved == 0u);
    CHECK(violations == 0u);
}

TEST_CASE("sphere terrain selection: the drawn set covers the sphere once and is balanced")
{
    const FlatPool pool;
    for (const f64 altitude : {2.0e7, 1.0e4, 2.0})
    {
        std::unordered_set<u64> splits;
        const SphereTerrainSelectionResult result = Settle(pool, Above(altitude), splits);

        std::unordered_set<u64> drawn;
        f64 solidAngle = 0.0;
        for (const SphereTerrainDrawnTile& tile : result.Drawn)
        {
            drawn.insert(tile.Key.Value);
            solidAngle += TileSolidAngle(tile.Key);
        }
        u32 nested = 0;
        for (const SphereTerrainDrawnTile& tile : result.Drawn)
        {
            for (CubeSphereTileKey up = tile.Key; up.GetLevel() > 0;)
            {
                up = up.GetParent();
                nested += drawn.contains(up.Value) ? 1u : 0u;
            }
        }
        CHECK(std::abs(solidAngle - (4.0 * std::numbers::pi)) < 1e-9);
        CHECK(nested == 0u);
        CHECK(Inspect(result.Drawn, Above(altitude)).Unbalanced == 0u);
    }
}

TEST_CASE("sphere terrain selection: a withheld tile's children hold its neighbours unsplit")
{
    FlatPool pool;
    std::unordered_set<u64> splits;
    const dvec3 eye = Above(50.0);
    const SphereTerrainSelectionResult full = Settle(pool, eye, splits);

    // Withhold the children of a level-12 tile beside the one under the eye, so the eye's side
    // deepens while its neighbour cannot.
    const CubeSphere::FacePoint under = CubeSphere::DirectionToFace(eye);
    const u32 n = 1u << 12;
    const auto cell = [n](const f64 c) { return static_cast<u32>((c + 1.0) * 0.5 * n); };
    const CubeSphereTileKey below =
        CubeSphereTileKey::Make(under.OnFace, 12, cell(under.U), cell(under.V));
    pool.Withheld = below.GetNeighbour(CubeSphere::Edge::PosU);

    std::unordered_set<u64> heldSplits;
    const SphereTerrainSelectionResult held = Settle(pool, eye, heldSplits);
    f64 solidAngle = 0.0;
    for (const SphereTerrainDrawnTile& tile : held.Drawn)
    {
        solidAngle += TileSolidAngle(tile.Key);
    }
    CHECK(DeepestLevel(full.Drawn) > 14u);
    CHECK(held.Stats.WaitingSplits > 0u);
    CHECK(std::abs(solidAngle - (4.0 * std::numbers::pi)) < 1e-9);
    CHECK(Inspect(held.Drawn, eye).Unbalanced == 0u);
}

TEST_CASE("sphere terrain selection: the drawn levels deepen toward the ground and hold still")
{
    const FlatPool pool;
    std::unordered_set<u64> splits;
    u32 previous = 0;
    u32 shallower = 0;
    f64 splitAltitude = 0.0;
    for (f64 altitude = 2.0e7; altitude > 2.0; altitude *= 0.8)
    {
        const vector<SphereTerrainDrawnTile> drawn = Settle(pool, Above(altitude), splits).Drawn;
        const u32 deepest = DeepestLevel(drawn);
        shallower += deepest < previous ? 1u : 0u;
        if (deepest > previous && previous >= 8 && splitAltitude == 0.0)
        {
            splitAltitude = altitude;
        }
        previous = deepest;
    }
    CHECK(shallower == 0u);
    CHECK(previous >= 20u);
    REQUIRE(splitAltitude > 0.0);

    // Oscillating a little about a split leaves the drawn set where it is.
    splits.clear();
    std::unordered_set<u64> settled;
    const vector<SphereTerrainDrawnTile> reference =
        Settle(pool, Above(splitAltitude), settled).Drawn;
    const auto keys = [](const vector<SphereTerrainDrawnTile>& drawn)
    {
        vector<u64> values;
        values.reserve(drawn.size());
        for (const SphereTerrainDrawnTile& tile : drawn)
        {
            values.push_back(tile.Key.Value);
        }
        std::ranges::sort(values);
        return values;
    };
    const vector<u64> referenceKeys = keys(reference);
    u32 changed = 0;
    for (u32 swing = 0; swing < 8; swing++)
    {
        const f64 altitude = splitAltitude * ((swing & 1u) != 0 ? 1.03 : 1.0);
        const SphereTerrainSelectionResult result =
            SelectSphereTerrain(FlatInput(pool, Above(altitude), &settled));
        settled = result.Splits;
        changed += keys(result.Drawn) == referenceKeys ? 0u : 1u;
    }
    CHECK(changed == 0u);
}

TEST_CASE("sphere terrain selection: adjacent levels meet with the coarse side unmorphed")
{
    const FlatPool pool;
    for (const f64 altitude : {2.0e7, 1.0e4, 2.0})
    {
        std::unordered_set<u64> splits;
        const dvec3 eye = Above(altitude);
        const SphereTerrainSelectionResult result = Settle(pool, eye, splits);
        const Adjacency adjacency = Inspect(result.Drawn, eye);
        CHECK(adjacency.Unbalanced == 0u);
        CHECK(adjacency.CoarseMorphed == 0u);
        CHECK(adjacency.FineUnmorphed == 0u);
        CHECK(adjacency.Visible > 0u);
    }
}

namespace
{
    // The drawn tile covering a body-fixed direction.
    const SphereTerrainDrawnTile* Covering(const vector<SphereTerrainDrawnTile>& drawn,
                                           const dvec3& direction)
    {
        const CubeSphere::FacePoint point = CubeSphere::DirectionToFace(direction);
        const u32 n = 1u << CubeSphere::MaxLevel;
        const auto cell = [n](const f64 c)
        { return std::min(static_cast<u32>((c + 1.0) * 0.5 * n), n - 1); };
        const CubeSphereTileKey deepest = CubeSphereTileKey::Make(
            point.OnFace, CubeSphere::MaxLevel, cell(point.U), cell(point.V));
        for (const SphereTerrainDrawnTile& tile : drawn)
        {
            if (tile.Key == deepest || tile.Key.IsAncestorOf(deepest))
            {
                return &tile;
            }
        }
        return nullptr;
    }

    // A view's side planes about the eye, body-fixed: a camera at the eye looking along forward.
    std::array<glm::dvec4, 6> SidePlanes(const dvec3& forward, const dvec3& up)
    {
        CameraView camera;
        camera.SetPerspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 1000.0f);
        camera.SetView(vec3(0.0f), vec3(forward), vec3(up));
        const Frustum frustum = Frustum::FromViewProjection(camera.ViewProjection());
        std::array<glm::dvec4, 6> planes{};
        planes.fill(glm::dvec4(0.0, 0.0, 0.0, 1.0));
        for (usize i = 0; i < 4; i++)
        {
            planes[i] = glm::dvec4(dvec3(vec3(frustum.Planes[i])), frustum.Planes[i].w);
        }
        return planes;
    }

    // Runs a budgeted, frustum-pruned selection until its splits stop changing.
    SphereTerrainSelectionResult SettleWith(const FlatPool& pool, const dvec3& eye,
                                            const std::array<glm::dvec4, 6>* frustum,
                                            const u32 budget)
    {
        std::unordered_set<u64> splits;
        SphereTerrainSelectionResult result;
        for (u32 frame = 0; frame < 4; frame++)
        {
            SphereTerrainSelectionInput input = FlatInput(pool, eye, &splits);
            input.Frustum = frustum;
            input.PinnedLevel = 2;
            input.TileBudget = budget;
            result = SelectSphereTerrain(input);
            if (result.Splits == splits)
            {
                break;
            }
            splits = result.Splits;
        }
        return result;
    }

    // Every coarse-fine edge in view whose coarse side was not held keeps the invariant.
    u32 UnheldMorphViolations(const vector<SphereTerrainDrawnTile>& drawn, const dvec3& eye)
    {
        std::unordered_map<u64, const SphereTerrainDrawnTile*> byKey;
        for (const SphereTerrainDrawnTile& tile : drawn)
        {
            byKey[tile.Key.Value] = &tile;
        }
        u32 violations = 0;
        for (const SphereTerrainDrawnTile& fine : drawn)
        {
            const CubeSphereTileGeometry geometry = GetCubeSphereTileGeometry(fine.Key);
            for (u32 e = 0; e < 4; e++)
            {
                const auto edge = static_cast<CubeSphere::Edge>(e);
                CubeSphereTileKey coarse = fine.Key.GetNeighbour(edge);
                if (byKey.contains(coarse.Value) || coarse.GetLevel() == 0)
                {
                    continue;
                }
                coarse = coarse.GetParent();
                if (!byKey.contains(coarse.Value) || byKey.at(coarse.Value)->Held ||
                    IsBoxBelowHorizon(ComputeSphereTerrainTileBox(coarse, Radius, 0.0f, 0.0f), eye,
                                      Radius))
                {
                    continue;
                }
                const SphereTerrainDrawnTile& outer = *byKey.at(coarse.Value);
                for (u32 k = 0; k <= SphereTerrainGridQuads; k++)
                {
                    const glm::dvec2 st =
                        EdgeParameter(edge, static_cast<f64>(k) / SphereTerrainGridQuads);
                    const f64 distance =
                        glm::length((CubeSphereTileDirection(geometry, st.x, st.y) * Radius) - eye);
                    violations +=
                        Morph(distance, outer) > 0.0 || Morph(distance, fine) < 1.0 ? 1u : 0u;
                }
            }
        }
        return violations;
    }
}

TEST_CASE("sphere terrain selection: a short budget keeps the ground under the eye at full depth")
{
    const FlatPool pool;
    const dvec3 eye = Above(2.0);
    const SphereTerrainSelectionResult full = SettleWith(pool, eye, nullptr, ~0u);
    // A tenth of what the unbudgeted selection wants: enough for the balanced chain of tiles
    // down to the deepest level under the eye (about 24 a level), not for the rings around it.
    constexpr u32 Budget = 600;
    const SphereTerrainSelectionResult short_ = SettleWith(pool, eye, nullptr, Budget);

    const SphereTerrainDrawnTile* fullUnder = Covering(full.Drawn, eye);
    const SphereTerrainDrawnTile* shortUnder = Covering(short_.Drawn, eye);
    REQUIRE(fullUnder != nullptr);
    REQUIRE(shortUnder != nullptr);
    CHECK(fullUnder->Key.GetLevel() >= 20u);
    CHECK(shortUnder->Key.GetLevel() == fullUnder->Key.GetLevel());
    CHECK(full.Stats.BudgetedTiles > 8 * Budget);
    CHECK(short_.Stats.HeldForBudget > 0u);
    CHECK(short_.Stats.BudgetedTiles <= Budget);

    f64 solidAngle = 0.0;
    for (const SphereTerrainDrawnTile& tile : short_.Drawn)
    {
        solidAngle += TileSolidAngle(tile.Key);
    }
    CHECK(std::abs(solidAngle - (4.0 * std::numbers::pi)) < 1e-9);
    CHECK(Inspect(short_.Drawn, eye).Unbalanced == 0u);
    CHECK(UnheldMorphViolations(short_.Drawn, eye) == 0u);
}

TEST_CASE("sphere terrain selection: beyond the near radius only the view refines")
{
    const FlatPool pool;
    const dvec3 up = glm::normalize(Above(1.0));
    const dvec3 north = glm::normalize(glm::cross(up, dvec3(0.0, 0.0, 1.0)));
    const dvec3 eye = Above(2.0);
    const std::array<glm::dvec4, 6> planes = SidePlanes(north, up);
    const SphereTerrainSelectionResult full = SettleWith(pool, eye, nullptr, ~0u);
    const SphereTerrainSelectionResult pruned = SettleWith(pool, eye, &planes, ~0u);

    // The ground near the eye keeps its depth whichever way it lies, and so does the ground ahead;
    // the ground behind, beyond the near radius, does not.
    const f64 near = 0.5 * SphereTerrainSelectionSettings{}.OmnidirectionalRadius;
    u32 nearShallower = 0;
    for (const dvec3& side : {north, -north, glm::cross(up, north), -glm::cross(up, north)})
    {
        const dvec3 at = glm::normalize(eye + (side * near));
        nearShallower +=
            Covering(pruned.Drawn, at)->Key.GetLevel() < Covering(full.Drawn, at)->Key.GetLevel()
                ? 1u
                : 0u;
    }
    CHECK(nearShallower == 0u);
    const dvec3 ahead = glm::normalize(eye + (north * 2000.0));
    const dvec3 behind = glm::normalize(eye - (north * 2000.0));
    CHECK(Covering(pruned.Drawn, ahead)->Key.GetLevel() ==
          Covering(full.Drawn, ahead)->Key.GetLevel());
    CHECK(Covering(pruned.Drawn, behind)->Key.GetLevel() <
          Covering(full.Drawn, behind)->Key.GetLevel());
    CHECK(pruned.Stats.HeldForFrustum > 0u);
    CHECK(pruned.Drawn.size() < full.Drawn.size());

    f64 solidAngle = 0.0;
    for (const SphereTerrainDrawnTile& tile : pruned.Drawn)
    {
        solidAngle += TileSolidAngle(tile.Key);
    }
    CHECK(std::abs(solidAngle - (4.0 * std::numbers::pi)) < 1e-9);
    CHECK(Inspect(pruned.Drawn, eye).Unbalanced == 0u);
    CHECK(UnheldMorphViolations(pruned.Drawn, eye) == 0u);
}
