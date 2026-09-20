// The material parameter arena's allocator: granule rounding, first-fit placement, the deferred
// release window, and coalescing at reclaim rather than at release. Device-free — the allocator
// holds free runs and counters and touches no buffer, which is the whole reason it is its own type.

#include <doctest/doctest.h>

#include "Renderer/Backend/MaterialArena.h"

#include <array>
#include <utility>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr u32 Granule = MaterialArena::GranuleBytes;
    constexpr u32 FramesInFlight = 2;

    // Advances the frame cycle, which is what reclaims a parked range: a release taken while
    // frame f was current comes back the next time f is acquired.
    void CycleFrames(MaterialArena& arena, u32 frames)
    {
        for (u32 i = 0; i < frames; ++i)
        {
            arena.OnFrameAcquired(i % FramesInFlight);
        }
    }
}

TEST_CASE(
    "Material arena: every allocation is a whole granule, and a fieldless block still takes one")
{
    // The granule is what makes first fit exact here, so the rounding is the property rather than
    // an implementation detail: a request of any size occupies a granule multiple, and a zero-byte
    // block occupies one rather than nothing — two of those sharing an offset would make two
    // materials' handles equal.
    CHECK(MaterialArena::OccupiedBytes(0) == Granule);
    CHECK(MaterialArena::OccupiedBytes(1) == Granule);
    CHECK(MaterialArena::OccupiedBytes(Granule) == Granule);
    CHECK(MaterialArena::OccupiedBytes(Granule + 1) == 2 * Granule);
    CHECK(MaterialArena::OccupiedBytes(1280) == 5 * Granule);

    MaterialArena arena;
    arena.Init(16 * Granule, FramesInFlight);
    const optional<u32> first = arena.Allocate(0);
    const optional<u32> second = arena.Allocate(0);
    REQUIRE(first.has_value());
    REQUIRE(second.has_value());
    CHECK(*first != *second);
    CHECK(arena.GetLiveBytes() == 2 * Granule);
    CHECK(arena.GetLiveBlocks() == 2u);
    CHECK(arena.GetFreeBytes() == 14 * Granule);
}

TEST_CASE("Material arena: a freed range is reusable only after its window expires, and coalesces "
          "at that reclaim")
{
    MaterialArena arena;
    arena.Init(8 * Granule, FramesInFlight);

    const optional<u32> a = arena.Allocate(Granule);
    const optional<u32> b = arena.Allocate(Granule);
    const optional<u32> c = arena.Allocate(Granule);
    REQUIRE(a.has_value());
    REQUIRE(b.has_value());
    REQUIRE(c.has_value());

    // Release the middle two, in the order that leaves three adjacent free ranges once the tail is
    // counted — the case a merge at Release would collapse straight away.
    arena.ReleaseDeferred(*a, Granule, 0);
    arena.ReleaseDeferred(*b, Granule, 0);

    // Still parked: the bytes are neither live nor free, and the untouched tail is the only run.
    CHECK(arena.GetLiveBytes() == Granule);
    CHECK(arena.GetFreeBytes() == 5 * Granule);
    CHECK(arena.GetLargestFreeRun() == 5 * Granule);
    CHECK(arena.GetFreeRuns().size() == 1u);

    // A request the parked bytes would serve is refused while the window stands, which is the
    // window doing its job rather than an allocator that is merely slow to merge.
    CHECK_FALSE(arena.Allocate(6 * Granule).has_value());

    CycleFrames(arena, FramesInFlight);

    // Reclaimed and merged with each other; the third allocation still splits the arena in two.
    CHECK(arena.GetFreeBytes() == 7 * Granule);
    CHECK(arena.GetFreeRuns().size() == 2u);
    CHECK(arena.GetLargestFreeRun() == 5 * Granule);

    arena.ReleaseDeferred(*c, Granule, 0);
    CycleFrames(arena, FramesInFlight);

    // With the last hole filled the arena is one run again — the neighbours on both sides merged.
    CHECK(arena.GetFreeRuns().size() == 1u);
    CHECK(arena.GetLargestFreeRun() == 8 * Granule);
    CHECK(arena.GetLiveBlocks() == 0u);
}

TEST_CASE("Material arena: mixed-size churn leaves the bytes it reports usable")
{
    // The claim-and-release pool is the workload first fit is supposed to be bad at: several
    // classes of different sizes, cycling. The granule is what earns the claim, so the case
    // asserts the outcome rather than the premise — after the churn, a request the arena has the
    // bytes for still succeeds.
    constexpr u32 Arena = 64 * Granule;
    MaterialArena arena;
    arena.Init(Arena, FramesInFlight);

    constexpr std::array Classes{1u * Granule, 3u * Granule, 5u * Granule};
    vector<std::pair<u32, u32>> live;
    u32 frame = 0;

    bool everyClaimServed = true;
    for (u32 round = 0; round < 12; ++round)
    {
        // Claim one of each class, then drop the two oldest, so the holes left behind are of
        // mixed sizes and in scattered positions rather than at the arena's end.
        for (const u32 bytes : Classes)
        {
            const optional<u32> offset = arena.Allocate(bytes);
            everyClaimServed = everyClaimServed && offset.has_value();
            if (offset)
            {
                live.emplace_back(*offset, bytes);
            }
        }
        for (u32 i = 0; i < 2 && !live.empty(); ++i)
        {
            const auto [offset, bytes] = live.front();
            live.erase(live.begin());
            arena.ReleaseDeferred(offset, bytes, frame % FramesInFlight);
        }
        ++frame;
        arena.OnFrameAcquired(frame % FramesInFlight);
    }

    // The property, asserted once over the whole churn: the arena kept serving requests it had
    // the bytes for. A first fit without the granule is what would start refusing them.
    CHECK(everyClaimServed);

    for (const auto [offset, bytes] : live)
    {
        arena.ReleaseDeferred(offset, bytes, frame % FramesInFlight);
    }
    CycleFrames(arena, 2 * FramesInFlight);

    // Everything handed back, and the arena is whole again rather than a gravel of runs.
    CHECK(arena.GetLiveBlocks() == 0u);
    CHECK(arena.GetFreeBytes() == Arena);
    CHECK(arena.GetLargestFreeRun() == Arena);
    CHECK(arena.GetFreeRuns().size() == 1u);
}

TEST_CASE("Material arena: live allocations never overlap")
{
    // The property a packed arena has to keep and a fixed-slot table got for free: no byte belongs
    // to two materials at once, across a churn that reuses ranges repeatedly.
    MaterialArena arena;
    arena.Init(32 * Granule, FramesInFlight);

    vector<std::pair<u32, u32>> live;
    u32 frame = 0;
    u32 overlaps = 0;
    for (u32 round = 0; round < 20; ++round)
    {
        const u32 bytes = (1u + (round % 4u)) * Granule;
        const optional<u32> offset = arena.Allocate(bytes);
        REQUIRE(offset.has_value());

        for (const auto [otherOffset, otherBytes] : live)
        {
            if (*offset < otherOffset + otherBytes && otherOffset < *offset + bytes)
            {
                ++overlaps;
            }
        }
        live.emplace_back(*offset, bytes);

        if (live.size() > 4)
        {
            const auto [oldOffset, oldBytes] = live.front();
            live.erase(live.begin());
            arena.ReleaseDeferred(oldOffset, oldBytes, frame % FramesInFlight);
        }
        ++frame;
        arena.OnFrameAcquired(frame % FramesInFlight);
    }
    CHECK(overlaps == 0u);
}

TEST_CASE("Material arena: an over-large request is refused rather than served short")
{
    MaterialArena arena;
    arena.Init(4 * Granule, FramesInFlight);

    const optional<u32> held = arena.Allocate(2 * Granule);
    REQUIRE(held.has_value());
    CHECK_FALSE(arena.Allocate(3 * Granule).has_value());

    // The figures the exhaustion fatal names are readable from the arena, which is what makes the
    // message actionable rather than "the table is full".
    CHECK(arena.GetArenaBytes() == 4 * Granule);
    CHECK(arena.GetLiveBytes() == 2 * Granule);
    CHECK(arena.GetLiveBlocks() == 1u);
    CHECK(arena.GetLargestFreeRun() == 2 * Granule);
}
