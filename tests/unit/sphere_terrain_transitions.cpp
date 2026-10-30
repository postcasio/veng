// Sphere terrain arrivals and departures, device-free: a tile appearing under one drawn last frame
// morphs in from its parent's shape, its floor falling monotonically from 1 to 0 within the arrival
// time; a coarser tile replacing its children waits while they morph out into its shape, their
// floor rising monotonically from 0 to 1 within the departure time, and the drawn set covers the
// region once throughout; a tile handed from one direction to the other keeps its floor; a
// departure whose tile leaves the pool ends at once; and a tile in neither draws with no floor.

#include <algorithm>
#include <array>
#include <unordered_set>

#include <doctest/doctest.h>

#include <Veng/Math/CubeSphere.h>

#include <Renderer/SphereTerrainResidency.h>
#include <Renderer/SphereTerrainTransitions.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr f32 Delta = 1.0f / 60.0f;

    // Every tile resident at a layer of its own, except those named missing.
    class Pool final : public SphereTerrainTileOracle
    {
    public:
        const SphereTerrainTileRecord* Find(const CubeSphereTileKey key) const override
        {
            if (Missing.contains(key.Value))
            {
                return nullptr;
            }
            m_Record = {
                .Key = key, .Layer = LayerOf(key), .State = SphereTerrainTileState::Resident};
            return &m_Record;
        }

        static u32 LayerOf(const CubeSphereTileKey key)
        {
            return (key.GetLevel() * 1000u) + (key.GetY() * 64u) + key.GetX();
        }

        std::unordered_set<u64> Missing;

    private:
        mutable SphereTerrainTileRecord m_Record;
    };

    const CubeSphereTileKey Coarse = CubeSphereTileKey::Make(CubeSphere::Face::PosX, 3, 2, 2);
    const CubeSphereTileKey Bystander = CubeSphereTileKey::Make(CubeSphere::Face::PosX, 3, 5, 5);

    SphereTerrainDrawnTile Drawn(const CubeSphereTileKey key)
    {
        return {.Key = key, .Layer = Pool::LayerOf(key)};
    }

    vector<SphereTerrainDrawnTile> CoarseSet()
    {
        return {Drawn(Coarse), Drawn(Bystander)};
    }

    vector<SphereTerrainDrawnTile> FineSet()
    {
        vector<SphereTerrainDrawnTile> set{Drawn(Bystander)};
        for (u32 c = 0; c < 4; c++)
        {
            set.push_back(Drawn(Coarse.GetChild(c)));
        }
        return set;
    }

    bool Holds(const SphereTerrainTransitionFrame& frame, const CubeSphereTileKey key)
    {
        return std::ranges::any_of(frame.Drawn, [&](const SphereTerrainDrawnTile& tile)
                                   { return tile.Key == key; });
    }

    f32 FloorOf(const SphereTerrainTransitionFrame& frame, const CubeSphereTileKey key)
    {
        const auto it = frame.Floors.find(key.Value);
        return it != frame.Floors.end() ? it->second : 0.0f;
    }

    // Whether the region under the coarse tile is drawn once: the tile or all four children.
    bool CoversOnce(const SphereTerrainTransitionFrame& frame)
    {
        u32 children = 0;
        for (u32 c = 0; c < 4; c++)
        {
            children += Holds(frame, Coarse.GetChild(c)) ? 1u : 0u;
        }
        return Holds(frame, Coarse) ? children == 0 : children == 4;
    }
}

TEST_CASE("sphere terrain transitions: a coarser tile waits while its children morph out")
{
    const Pool pool;
    SphereTerrainTransitionState state;
    (void)AdvanceSphereTerrainTransitions(state, FineSet(), Delta, pool);

    const CubeSphereTileKey child = Coarse.GetChild(2);
    f32 previous = -1.0f;
    f32 firstFloor = -1.0f;
    f32 lastFloor = 0.0f;
    f32 largestFall = 0.0f;
    f32 bystanderFloor = 0.0f;
    u32 departingFrames = 0;
    u32 coverFaults = 0;
    u32 layerFaults = 0;
    u32 framesToCoarse = 0;
    for (u32 frame = 1; frame <= 60; frame++)
    {
        const SphereTerrainTransitionFrame result =
            AdvanceSphereTerrainTransitions(state, CoarseSet(), Delta, pool);
        coverFaults += CoversOnce(result) ? 0u : 1u;
        bystanderFloor = std::max(bystanderFloor, FloorOf(result, Bystander));
        if (Holds(result, Coarse))
        {
            framesToCoarse = frame;
            break;
        }
        const f32 floor = FloorOf(result, child);
        firstFloor = firstFloor < 0.0f ? floor : firstFloor;
        largestFall = std::max(largestFall, previous - floor);
        previous = floor;
        lastFloor = floor;
        departingFrames += result.Departing.size() == 4 ? 1u : 0u;
        for (const SphereTerrainDrawnTile& tile : result.Drawn)
        {
            layerFaults += tile.Layer == Pool::LayerOf(tile.Key) &&
                                   (tile.Key.GetLevel() == 0 ||
                                    tile.ParentLayer == Pool::LayerOf(tile.Key.GetParent()) ||
                                    tile.Key == Bystander)
                               ? 0u
                               : 1u;
        }
    }
    CHECK(firstFloor == doctest::Approx(0.0f));
    CHECK(largestFall <= 0.0f);
    CHECK(lastFloor == doctest::Approx(1.0f));
    CHECK(framesToCoarse > 1);
    CHECK(static_cast<f32>(framesToCoarse - 1) * Delta <=
          SphereTerrainDepartureSeconds + (1.5f * Delta));
    CHECK(departingFrames == framesToCoarse - 1);
    CHECK(coverFaults == 0);
    CHECK(layerFaults == 0);
    CHECK(bystanderFloor == 0.0f);

    // Once the coarser tile draws, its children are gone and nothing carries a floor.
    const SphereTerrainTransitionFrame settled =
        AdvanceSphereTerrainTransitions(state, CoarseSet(), Delta, pool);
    CHECK(settled.Drawn.size() == 2);
    CHECK(settled.Floors.empty());
    CHECK(settled.Departing.empty());
}

TEST_CASE("sphere terrain transitions: a split's children morph in from their parent's shape")
{
    const Pool pool;
    SphereTerrainTransitionState state;
    (void)AdvanceSphereTerrainTransitions(state, CoarseSet(), Delta, pool);

    const CubeSphereTileKey child = Coarse.GetChild(1);
    f32 previous = 2.0f;
    f32 firstFloor = -1.0f;
    f32 largestRise = 0.0f;
    u32 floored = 0;
    for (u32 frame = 1; frame <= 60; frame++)
    {
        const SphereTerrainTransitionFrame result =
            AdvanceSphereTerrainTransitions(state, FineSet(), Delta, pool);
        const f32 floor = FloorOf(result, child);
        firstFloor = firstFloor < 0.0f ? floor : firstFloor;
        largestRise = std::max(largestRise, floor - previous);
        previous = floor;
        floored += floor > 0.0f ? 1u : 0u;
    }
    CHECK(firstFloor == doctest::Approx(1.0f));
    CHECK(largestRise <= 0.0f);
    CHECK(previous == 0.0f);
    CHECK(static_cast<f32>(floored) * Delta <= SphereTerrainArrivalSeconds + Delta);
}

TEST_CASE("sphere terrain transitions: a tile changing direction keeps its floor")
{
    const Pool pool;
    SphereTerrainTransitionState state;
    const CubeSphereTileKey child = Coarse.GetChild(0);
    const f32 step = Delta / SphereTerrainDepartureSeconds;

    // Departing, then selected again: the arrival takes up the floor the departure reached.
    (void)AdvanceSphereTerrainTransitions(state, FineSet(), Delta, pool);
    f32 departed = 0.0f;
    for (u32 frame = 0; frame < 6; frame++)
    {
        departed = FloorOf(AdvanceSphereTerrainTransitions(state, CoarseSet(), Delta, pool), child);
    }
    const f32 returned =
        FloorOf(AdvanceSphereTerrainTransitions(state, FineSet(), Delta, pool), child);
    CHECK(departed > 0.0f);
    CHECK(std::abs(returned - departed) <= 1.01f * step);

    // Arriving, then replaced again: the departure takes up the floor the arrival reached.
    f32 arrived = returned;
    for (u32 frame = 0; frame < 2; frame++)
    {
        arrived = FloorOf(AdvanceSphereTerrainTransitions(state, FineSet(), Delta, pool), child);
    }
    const SphereTerrainTransitionFrame leaving =
        AdvanceSphereTerrainTransitions(state, CoarseSet(), Delta, pool);
    CHECK(arrived > 0.0f);
    CHECK_FALSE(Holds(leaving, Coarse));
    CHECK(std::abs(FloorOf(leaving, child) - arrived) <= 1.01f * step);
}

TEST_CASE("sphere terrain transitions: a departure whose tile leaves the pool ends at once")
{
    Pool pool;
    SphereTerrainTransitionState state;
    (void)AdvanceSphereTerrainTransitions(state, FineSet(), Delta, pool);
    const SphereTerrainTransitionFrame departing =
        AdvanceSphereTerrainTransitions(state, CoarseSet(), Delta, pool);
    CHECK_FALSE(Holds(departing, Coarse));

    pool.Missing.insert(Coarse.GetChild(3).Value);
    const SphereTerrainTransitionFrame ended =
        AdvanceSphereTerrainTransitions(state, CoarseSet(), Delta, pool);
    CHECK(Holds(ended, Coarse));
    CHECK(CoversOnce(ended));
    CHECK(ended.Floors.empty());
    CHECK(ended.Departing.empty());
}
