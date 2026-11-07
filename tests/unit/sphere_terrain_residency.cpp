// The sphere terrain pool's residency rules, device-free, driven by the selection with an instant
// generator: under a random walk of the eye the pool never exceeds its capacity, stays closed under
// parent, never evicts a pinned tile or one with a held child; a capacity change compacts what
// it keeps into the new range; a held tile's cached facts are its key's; the pending count tracks
// the pending tiles; and eviction takes the least recently wanted, ties on the key.

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

TEST_CASE("sphere terrain residency: a held tile's facts are its key's geometry")
{
    SphereTerrainResidency pool(64, 0, Radius);
    RequestPinned(pool);
    CompleteAll(pool);
    // A chain down one face to a deep level, and the pinned top across every face.
    vector<CubeSphereTileKey> keys;
    CubeSphereTileKey key = CubeSphereTileKey::Make(CubeSphere::Face::NegY, 0, 0, 0);
    for (u32 level = 1; level <= 20; level++)
    {
        key = key.GetChild(level % 4);
        REQUIRE(pool.Request(key).Outcome == SphereTerrainResidency::RequestOutcome::Accepted);
        CompleteAll(pool);
        keys.push_back(key);
    }
    for (u32 face = 0; face < CubeSphere::FaceCount; face++)
    {
        keys.push_back(CubeSphereTileKey::Make(static_cast<CubeSphere::Face>(face), 0, 0, 0));
    }
    u32 differ = 0;
    for (const CubeSphereTileKey held : keys)
    {
        const SphereTerrainTileRecord* record = pool.Find(held);
        REQUIRE(record != nullptr);
        REQUIRE(record->HasFacts);
        const CubeSphereTileGeometry fresh = GetCubeSphereTileGeometry(held);
        const CubeSphereTileGeometry& cached = record->Facts.Geometry;
        differ += cached.Corners != fresh.Corners || cached.CornerNorms != fresh.CornerNorms ||
                          cached.Center != fresh.Center || cached.ThetaSpan != fresh.ThetaSpan ||
                          cached.CubeStartU != fresh.CubeStartU ||
                          cached.CubeStartV != fresh.CubeStartV
                      ? 1u
                      : 0u;
        // The box a cached frame builds is the box built from the key.
        const SphereTerrainTileBox fromFrame =
            ComputeSphereTerrainTileBox(record->Facts.Frame, Radius, -10.0f, 40.0f);
        const SphereTerrainTileBox fromKey =
            ComputeSphereTerrainTileBox(held, Radius, -10.0f, 40.0f);
        differ += fromFrame.Min != fromKey.Min || fromFrame.Max != fromKey.Max ? 1u : 0u;
    }
    CHECK(differ == 0u);
}

TEST_CASE("sphere terrain residency: the pending count is the pending tiles")
{
    SphereTerrainResidency pool(256, PinnedLevel, Radius);
    // A level is requested once the one above it is resident.
    RequestPinned(pool);
    const auto Pending = [&]
    {
        return static_cast<u32>(std::ranges::count_if(
            pool.GetTiles(), [](const auto& entry)
            { return entry.second.State == SphereTerrainTileState::Pending; }));
    };
    u32 mismatches = 0;
    mismatches += pool.GetPendingCount() != Pending() ? 1u : 0u;
    CompleteAll(pool);
    RequestPinned(pool);
    mismatches += pool.GetPendingCount() != Pending() ? 1u : 0u;
    CompleteAll(pool);
    mismatches += pool.GetPendingCount() != Pending() ? 1u : 0u;
    REQUIRE(pool.GetPinnedResidentCount() == 30u);

    // A burst of requests under every pinned leaf, half completed, a few removed while pending.
    vector<CubeSphereTileKey> requested;
    for (const auto& [value, record] : pool.GetTiles())
    {
        if (record.Key.GetLevel() == PinnedLevel)
        {
            for (u32 child = 0; child < 4; child++)
            {
                requested.push_back(record.Key.GetChild(child));
            }
        }
    }
    for (const CubeSphereTileKey key : requested)
    {
        (void)pool.Request(key);
    }
    mismatches += pool.GetPendingCount() != Pending() ? 1u : 0u;
    CHECK(pool.GetPendingCount() == requested.size());
    for (usize i = 0; i < requested.size(); i++)
    {
        const SphereTerrainTileRecord* record = pool.Find(requested[i]);
        if (i % 2 == 0)
        {
            CHECK(pool.Complete(record->Key, record->Generation));
        }
        else if (i % 3 == 0)
        {
            CHECK(pool.Remove(record->Key));
        }
    }
    mismatches += pool.GetPendingCount() != Pending() ? 1u : 0u;
    CHECK(mismatches == 0u);
    CHECK(pool.GetPendingCount() > 0u);
}

TEST_CASE("sphere terrain residency: eviction takes the least recently wanted, ties on the key")
{
    // Room for the pinned top and the level-1 children of one face, no more.
    const u32 pinned = 6;
    SphereTerrainResidency pool(pinned + 4, 0, Radius);
    pool.BeginFrame(1);
    for (u32 face = 0; face < CubeSphere::FaceCount; face++)
    {
        (void)pool.Request(CubeSphereTileKey::Make(static_cast<CubeSphere::Face>(face), 0, 0, 0));
    }
    CompleteAll(pool);
    const CubeSphereTileKey root = CubeSphereTileKey::Make(CubeSphere::Face::PosZ, 0, 0, 0);
    for (u32 child = 0; child < 4; child++)
    {
        REQUIRE(pool.Request(root.GetChild(child)).Outcome ==
                SphereTerrainResidency::RequestOutcome::Accepted);
    }
    CompleteAll(pool);

    // Wanted this frame or the last: nothing is evictable, so a request waits.
    const CubeSphereTileKey other = CubeSphereTileKey::Make(CubeSphere::Face::NegX, 0, 0, 0);
    pool.BeginFrame(2);
    CHECK(pool.Request(other.GetChild(0)).Outcome ==
          SphereTerrainResidency::RequestOutcome::Deferred);

    // Two children wanted at frame 3, two left at frame 1: the older go first, the least key first.
    pool.BeginFrame(3);
    pool.MarkWanted(root.GetChild(1));
    pool.MarkWanted(root.GetChild(2));
    pool.BeginFrame(5);
    vector<u64> evicted;
    for (u32 i = 0; i < 4; i++)
    {
        const SphereTerrainResidency::RequestResult result = pool.Request(other.GetChild(i));
        REQUIRE(result.Outcome == SphereTerrainResidency::RequestOutcome::Accepted);
        REQUIRE(result.Evicted.IsValid());
        evicted.push_back(result.Evicted.Value);
    }
    const u64 low = std::min(root.GetChild(0).Value, root.GetChild(3).Value);
    const u64 high = std::max(root.GetChild(0).Value, root.GetChild(3).Value);
    const u64 lowLater = std::min(root.GetChild(1).Value, root.GetChild(2).Value);
    const u64 highLater = std::max(root.GetChild(1).Value, root.GetChild(2).Value);
    CHECK(evicted == vector<u64>{low, high, lowLater, highLater});
    CHECK(pool.GetPinnedResidentCount() == pinned);

    // A request never evicts the parent it is made under, and a parent goes only after its child.
    CompleteAll(pool);
    pool.BeginFrame(10);
    const CubeSphereTileKey parent = other.GetChild(0);
    const CubeSphereTileKey grandchild = parent.GetChild(3);
    const SphereTerrainResidency::RequestResult under = pool.Request(grandchild);
    REQUIRE(under.Outcome == SphereTerrainResidency::RequestOutcome::Accepted);
    CHECK(under.Evicted.Value ==
          std::min({other.GetChild(1).Value, other.GetChild(2).Value, other.GetChild(3).Value}));
    CompleteAll(pool);
    pool.BeginFrame(20);
    vector<u64> later;
    const CubeSphereTileKey third = CubeSphereTileKey::Make(CubeSphere::Face::PosY, 0, 0, 0);
    for (u32 i = 0; i < 4; i++)
    {
        const SphereTerrainResidency::RequestResult result = pool.Request(third.GetChild(i));
        REQUIRE(result.Outcome == SphereTerrainResidency::RequestOutcome::Accepted);
        later.push_back(result.Evicted.Value);
    }
    const auto at = [&](const CubeSphereTileKey key)
    { return std::ranges::find(later, key.Value) - later.begin(); };
    CHECK(at(grandchild) < at(parent));
}
