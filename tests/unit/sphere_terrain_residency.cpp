// The sphere terrain pool's residency rules, device-free, driven by the selection with an instant
// generator: under a random walk of the eye the pool never exceeds its capacity, stays closed under
// parent, never evicts a pinned tile or one with a held child; and a capacity change compacts what
// it keeps into the new range.

#include <algorithm>
#include <random>
#include <unordered_set>

#include <doctest/doctest.h>

#include <Veng/Math/CubeSphere.h>

#include <Renderer/SphereTerrainResidency.h>
#include <Renderer/SphereTerrainSelection.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr f64 Radius = 1.0e5;
    constexpr u32 PinnedLevel = 1;

    void RequestPinned(SphereTerrainResidency& pool)
    {
        for (u32 level = 0; level <= PinnedLevel; level++)
        {
            const u32 n = 1u << level;
            for (u32 face = 0; face < CubeSphere::FaceCount; face++)
            {
                for (u32 y = 0; y < n; y++)
                {
                    for (u32 x = 0; x < n; x++)
                    {
                        (void)pool.Request(CubeSphereTileKey::Make(
                            static_cast<CubeSphere::Face>(face), level, x, y));
                    }
                }
            }
        }
    }

    // Every pending tile completes, measured flat, as an instant generator would.
    void CompleteAll(SphereTerrainResidency& pool)
    {
        vector<std::pair<CubeSphereTileKey, u32>> pending;
        for (const auto& [value, record] : pool.GetTiles())
        {
            if (record.State == SphereTerrainTileState::Pending)
            {
                pending.emplace_back(record.Key, record.Generation);
            }
        }
        for (const auto& [key, generation] : pending)
        {
            CHECK(pool.Complete(key, generation));
            pool.SetBound(key, generation, {.Measured = true});
        }
    }

    u32 CountOpenParents(const SphereTerrainResidency& pool)
    {
        u32 open = 0;
        for (const auto& [value, record] : pool.GetTiles())
        {
            if (record.Key.GetLevel() > 0 && !pool.IsResident(record.Key.GetParent()))
            {
                open++;
            }
        }
        return open;
    }
}

TEST_CASE("sphere terrain residency: a random walk stays bounded and closed under parent")
{
    constexpr u32 Capacity = 64;
    SphereTerrainResidency pool(Capacity, PinnedLevel);
    std::mt19937 random(7);
    std::uniform_real_distribution<f64> unit(-1.0, 1.0);

    dvec3 direction = glm::normalize(dvec3(0.4, 0.7, -0.2));
    f64 altitude = 2.0e4;
    std::unordered_set<u64> splits;
    u32 maxHeld = 0;
    u32 openParents = 0;
    u32 pinnedEvicted = 0;
    u32 evictedWithChild = 0;
    u64 frame = 1;
    for (u32 step = 0; step < 200; step++, frame++)
    {
        pool.BeginFrame(frame);
        CompleteAll(pool);
        RequestPinned(pool);

        // Strides wide enough that what the eye wanted a few steps ago is no longer wanted.
        direction =
            glm::normalize(direction + dvec3(unit(random), unit(random), unit(random)) * 0.4);
        altitude = std::clamp(altitude * (1.0 + (0.3 * unit(random))), 5.0, 5.0e5);
        const SphereTerrainSelectionResult result = SelectSphereTerrain({
            .Radius = Radius,
            .Eye = direction * (Radius + altitude),
            .PixelsPerRadian = 1000.0,
            .Tiles = &pool,
            .PreviousSplits = &splits,
        });
        splits = result.Splits;
        for (const CubeSphereTileKey key : result.Wanted)
        {
            pool.MarkWanted(key);
        }
        for (const SphereTerrainSelectionRequest& request : result.Requests)
        {
            const SphereTerrainResidency::RequestResult outcome = pool.Request(request.Key);
            if (outcome.Evicted.IsValid())
            {
                pinnedEvicted += outcome.Evicted.GetLevel() <= PinnedLevel ? 1u : 0u;
                for (const auto& [value, record] : pool.GetTiles())
                {
                    evictedWithChild +=
                        record.Key.GetLevel() > 0 && record.Key.GetParent() == outcome.Evicted ? 1u
                                                                                               : 0u;
                }
            }
        }
        for (const CubeSphereTileKey key : pool.CollectStalePending())
        {
            pool.Remove(key);
        }
        maxHeld = std::max(maxHeld, pool.GetHeldCount());
        openParents += CountOpenParents(pool);
    }

    CHECK(maxHeld <= Capacity);
    CHECK(maxHeld == Capacity);
    CHECK(pool.GetEvictions() > 0u);
    CHECK(openParents == 0u);
    CHECK(pinnedEvicted == 0u);
    CHECK(evictedWithChild == 0u);
    CHECK(pool.GetPinnedResidentCount() == 30u);
}

TEST_CASE("sphere terrain residency: a capacity change keeps the pinned top and compacts layers")
{
    SphereTerrainResidency pool(40, PinnedLevel);
    pool.BeginFrame(1);
    // A level is requested only under a resident parent, so the pinned top fills level by level.
    for (u32 level = 0; level <= PinnedLevel; level++)
    {
        RequestPinned(pool);
        CompleteAll(pool);
    }
    const CubeSphereTileKey parent = CubeSphereTileKey::Make(CubeSphere::Face::PosZ, 1, 1, 0);
    for (u32 child = 0; child < 4; child++)
    {
        CHECK(pool.Request(parent.GetChild(child)).Outcome ==
              SphereTerrainResidency::RequestOutcome::Accepted);
    }
    CompleteAll(pool);
    CHECK(pool.GetHeldCount() == 34u);

    // Grow: nothing moves.
    const SphereTerrainResidency::CapacityChange grown = pool.SetCapacity(100);
    CHECK(grown.Capacity == 100u);
    CHECK(grown.Moves.empty());

    // Shrink below what is held, three frames later so the children are no longer wanted: the
    // unwanted children go, and what is left fits the new range on distinct layers.
    pool.BeginFrame(4);
    const SphereTerrainResidency::CapacityChange shrunk = pool.SetCapacity(31);
    CHECK(shrunk.Capacity == 31u);
    CHECK(shrunk.Evicted.size() == 3u);
    std::unordered_set<u32> layers;
    u32 outside = 0;
    for (const auto& [value, record] : pool.GetTiles())
    {
        layers.insert(record.Layer);
        outside += record.Layer >= shrunk.Capacity ? 1u : 0u;
    }
    CHECK(layers.size() == pool.GetHeldCount());
    CHECK(outside == 0u);
    CHECK(pool.GetPinnedResidentCount() == 30u);

    // A shrink below the pinned top stops at it.
    const SphereTerrainResidency::CapacityChange floor = pool.SetCapacity(10);
    CHECK(floor.Capacity == pool.GetHeldCount());
    CHECK(pool.GetPinnedResidentCount() == 30u);
}
