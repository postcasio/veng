// Sphere terrain selection, device-free, over a pool that holds every tile of a flat sphere: the
// level range halves exactly per level and holds the neighbour invariant's inequality at any field
// of view; the drawn set covers the sphere once, is 2:1 balanced (also when a tile's children are
// withheld), deepens monotonically toward the ground and holds still inside the hysteresis band;
// and every edge between drawn tiles of adjacent levels has the coarse side unmorphed and the fine
// side fully morphed.

#include <algorithm>
#include <cmath>
#include <numbers>
#include <unordered_map>
#include <unordered_set>

#include <doctest/doctest.h>

#include <Veng/Math/CubeSphere.h>

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
