// Transient-allocation unit cases. AssignTransientSlots lives in
// Backend/TransientAllocation.h, the pure live-range/slot-assignment rule
// separate from the device-coupled compile in RenderGraph.cpp, so it is
// testable without a GPU. These are pure data -> data; no Context, no driver.

#include <doctest/doctest.h>

#include <Veng/Renderer/Backend/TransientAllocation.h>

using namespace Veng;
using namespace Veng::Renderer;
using namespace Veng::Renderer::Backend;

namespace
{
    // A baseline size class. Disjoint same-key transients may share; cases that
    // test the key dimension perturb one field.
    const AllocationKey KeyA{
        .Format = Format::RGBA8Unorm,
        .Extent = {64, 64},
        .Usage = ImageUsage::ColorAttachment,
    };

    // The number of distinct slots an assignment uses.
    [[nodiscard]] u32 SlotCount(const vector<u32>& assignment)
    {
        u32 count = 0;
        for (const u32 slot : assignment)
            count = std::max(count, slot + 1);
        return count;
    }
}

TEST_CASE("AssignTransientSlots: disjoint + same key share a slot")
{
    // A: passes 0..1, B: passes 2..3 — non-overlapping, equal key.
    const vector<TransientLifetime> lifetimes{{0, 1}, {2, 3}};
    const vector<AllocationKey> keys{KeyA, KeyA};

    const auto slots = AssignTransientSlots(lifetimes, keys);

    REQUIRE(slots.size() == 2);
    CHECK(slots[0] == slots[1]);
    CHECK(SlotCount(slots) == 1);
}

TEST_CASE("AssignTransientSlots: overlapping lifetimes get distinct slots")
{
    SUBCASE("touching at an endpoint overlaps")
    {
        // A ends at pass 2, B starts at pass 2 — they share pass 2.
        const vector<TransientLifetime> lifetimes{{0, 2}, {2, 3}};
        const vector<AllocationKey> keys{KeyA, KeyA};

        const auto slots = AssignTransientSlots(lifetimes, keys);

        CHECK(slots[0] != slots[1]);
        CHECK(SlotCount(slots) == 2);
    }

    SUBCASE("nested overlaps")
    {
        // B (1..2) lives entirely inside A (0..3).
        const vector<TransientLifetime> lifetimes{{0, 3}, {1, 2}};
        const vector<AllocationKey> keys{KeyA, KeyA};

        const auto slots = AssignTransientSlots(lifetimes, keys);

        CHECK(slots[0] != slots[1]);
        CHECK(SlotCount(slots) == 2);
    }
}

TEST_CASE("AssignTransientSlots: key mismatch blocks sharing despite disjoint lifetimes")
{
    // Disjoint ranges, so only the key dimension can force distinct slots.
    const vector<TransientLifetime> lifetimes{{0, 1}, {2, 3}};

    SUBCASE("format differs")
    {
        AllocationKey other = KeyA;
        other.Format = Format::R8Unorm;
        const auto slots = AssignTransientSlots(lifetimes, {KeyA, other});
        CHECK(slots[0] != slots[1]);
        CHECK(SlotCount(slots) == 2);
    }

    SUBCASE("extent differs")
    {
        AllocationKey other = KeyA;
        other.Extent = {32, 64};
        const auto slots = AssignTransientSlots(lifetimes, {KeyA, other});
        CHECK(slots[0] != slots[1]);
        CHECK(SlotCount(slots) == 2);
    }

    SUBCASE("usage differs")
    {
        AllocationKey other = KeyA;
        other.Usage = ImageUsage::Sampled;
        const auto slots = AssignTransientSlots(lifetimes, {KeyA, other});
        CHECK(slots[0] != slots[1]);
        CHECK(SlotCount(slots) == 2);
    }
}

TEST_CASE("AssignTransientSlots: a chain of disjoint same-key transients collapses to one slot")
{
    // A (0..1), B (2..3), C (4..5): each starts after the previous ends, so all
    // three reuse one slot.
    const vector<TransientLifetime> lifetimes{{0, 1}, {2, 3}, {4, 5}};
    const vector<AllocationKey> keys{KeyA, KeyA, KeyA};

    const auto slots = AssignTransientSlots(lifetimes, keys);

    CHECK(slots[0] == slots[1]);
    CHECK(slots[1] == slots[2]);
    CHECK(SlotCount(slots) == 1);
}

TEST_CASE("AssignTransientSlots: an overlapping pair needs two slots, minimally")
{
    // A (0..3) and B (1..2) overlap — two slots, and only two.
    const vector<TransientLifetime> lifetimes{{0, 3}, {1, 2}};
    const vector<AllocationKey> keys{KeyA, KeyA};

    const auto slots = AssignTransientSlots(lifetimes, keys);

    CHECK(slots[0] != slots[1]);
    CHECK(SlotCount(slots) == 2);
}

TEST_CASE(
    "AssignTransientSlots: a write-only transient (FirstUse == LastUse) shares like any other")
{
    // A is written but never read (e.g. a DontCare depth buffer): LastUse ==
    // FirstUse. B starts after, same key, so they still share.
    const vector<TransientLifetime> lifetimes{{0, 0}, {1, 2}};
    const vector<AllocationKey> keys{KeyA, KeyA};

    const auto slots = AssignTransientSlots(lifetimes, keys);

    CHECK(slots[0] == slots[1]);
    CHECK(SlotCount(slots) == 1);
}

TEST_CASE("AssignTransientSlots: two write-only transients at the same pass overlap")
{
    // Both live only at pass 0 — same index overlaps itself, so distinct slots.
    const vector<TransientLifetime> lifetimes{{0, 0}, {0, 0}};
    const vector<AllocationKey> keys{KeyA, KeyA};

    const auto slots = AssignTransientSlots(lifetimes, keys);

    CHECK(slots[0] != slots[1]);
    CHECK(SlotCount(slots) == 2);
}

TEST_CASE("AssignTransientSlots: input order independent of FirstUse order")
{
    // The same disjoint chain presented out of FirstUse order still collapses to
    // one slot, and the returned indices stay parallel to the input.
    const vector<TransientLifetime> lifetimes{{4, 5}, {0, 1}, {2, 3}};
    const vector<AllocationKey> keys{KeyA, KeyA, KeyA};

    const auto slots = AssignTransientSlots(lifetimes, keys);

    REQUIRE(slots.size() == 3);
    CHECK(slots[0] == slots[1]);
    CHECK(slots[1] == slots[2]);
    CHECK(SlotCount(slots) == 1);
}
