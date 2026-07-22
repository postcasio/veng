// Draw-budget unit cases. DrawBudget owns the per-frame draw-slot and skinning-palette
// budgets the three gather phases claim through, with no device and no I/O, so the clamp
// policy is pinned here rather than only through a rendered image.

#include <doctest/doctest.h>

#include "Renderer/DrawBudget.h"

using namespace Veng;
using namespace Veng::Renderer;

TEST_CASE("draw budget: a zero-slot budget refuses the first claim")
{
    DrawBudget budget(0, 64);

    u32 slot = 0xFFFFFFFFU;
    CHECK_FALSE(budget.TryClaimSlot(slot));
    CHECK(slot == 0xFFFFFFFFU); // untouched, not slot 0
    CHECK(budget.GetSlotCursor() == 0);
    CHECK(budget.GetStats().SlotsGranted == 0);
    CHECK(budget.GetStats().SlotLimit == 0);
}

TEST_CASE("draw budget: exactly MaxSlots claims succeed and the next fails")
{
    constexpr u32 MaxSlots = 4;
    DrawBudget budget(MaxSlots, 0);

    for (u32 i = 0; i < MaxSlots; ++i)
    {
        u32 slot = 0;
        CHECK(budget.TryClaimSlot(slot));
    }

    u32 overflow = 0;
    CHECK_FALSE(budget.TryClaimSlot(overflow));
    CHECK(budget.GetSlotCursor() == MaxSlots);
    CHECK(budget.GetStats().SlotsGranted == MaxSlots);
}

TEST_CASE("draw budget: granted slots are contiguous from 0 and the cursor ends at the limit")
{
    constexpr u32 MaxSlots = 8;
    DrawBudget budget(MaxSlots, 0);

    // Twice the budget in claims: the granted ones must still be 0..MaxSlots-1 with no gap,
    // which is the contiguity the GPU cull arrays index by.
    u32 expected = 0;
    for (u32 i = 0; i < MaxSlots * 2; ++i)
    {
        u32 slot = 0;
        if (budget.TryClaimSlot(slot))
        {
            CHECK(slot == expected);
            ++expected;
        }
    }

    CHECK(expected == MaxSlots);
    CHECK(budget.GetSlotCursor() == MaxSlots);
}

TEST_CASE("draw budget: one cursor is shared across the three phases")
{
    constexpr u32 MaxSlots = 5;
    DrawBudget budget(MaxSlots, 16);

    u32 slot = 0;
    CHECK(budget.TryClaimSlot(slot)); // static
    CHECK(slot == 0);
    CHECK(budget.TryClaimSlot(slot)); // static
    CHECK(slot == 1);

    u32 paletteBase = 0;
    CHECK(budget.TryClaimSkinnedDraw(4, slot, paletteBase) == SkinnedClaim::Granted);
    CHECK(slot == 2);
    CHECK(budget.TryClaimSkinnedDraw(4, slot, paletteBase) == SkinnedClaim::Granted);
    CHECK(slot == 3);

    CHECK(budget.TryClaimSlot(slot)); // translucent
    CHECK(slot == 4);

    // Every phase now sees the same exhausted budget.
    CHECK_FALSE(budget.TryClaimSlot(slot));
    CHECK(budget.TryClaimSkinnedDraw(1, slot, paletteBase) == SkinnedClaim::SlotsExhausted);
    CHECK(budget.GetSlotCursor() == MaxSlots);
    CHECK(budget.GetStats().SlotsGranted == MaxSlots);
}

TEST_CASE("draw budget: drops land in the recording phase's counter with their full magnitude")
{
    DrawBudget budget(0, 0);

    budget.RecordDropped(DrawPhase::StaticOpaque, 7);
    budget.RecordDropped(DrawPhase::Skinned, 40000);
    budget.RecordDropped(DrawPhase::Translucent, 3);

    const DrawBudgetStats& stats = budget.GetStats();
    CHECK(stats.StaticDropped == 7);
    CHECK(stats.SkinnedDropped == 40000);
    CHECK(stats.TranslucentDropped == 3);
    CHECK(stats.PaletteDropped == 0);
}

TEST_CASE("draw budget: a phase's drops accumulate rather than overwrite")
{
    DrawBudget budget(0, 0);

    budget.RecordDropped(DrawPhase::StaticOpaque, 2);
    budget.RecordDropped(DrawPhase::StaticOpaque, 5);

    CHECK(budget.GetStats().StaticDropped == 7);
    CHECK(budget.GetStats().SkinnedDropped == 0);
    CHECK(budget.GetStats().TranslucentDropped == 0);
}

TEST_CASE("draw budget: a failed skinned claim moves neither cursor")
{
    SUBCASE("slots available, palette exhausted")
    {
        DrawBudget budget(4, 3);

        u32 slot = 0;
        u32 paletteBase = 0;
        CHECK(budget.TryClaimSkinnedDraw(4, slot, paletteBase) == SkinnedClaim::PaletteExhausted);
        CHECK(budget.GetSlotCursor() == 0);
        CHECK(budget.GetPaletteCursor() == 0);
        CHECK(budget.GetStats().PaletteDropped == 1);
        CHECK(budget.GetStats().SlotsGranted == 0);
    }

    SUBCASE("palette available, slots exhausted")
    {
        DrawBudget budget(1, 64);

        u32 slot = 0;
        u32 paletteBase = 0;
        CHECK(budget.TryClaimSkinnedDraw(4, slot, paletteBase) == SkinnedClaim::Granted);
        CHECK(budget.GetSlotCursor() == 1);
        CHECK(budget.GetPaletteCursor() == 4);

        CHECK(budget.TryClaimSkinnedDraw(4, slot, paletteBase) == SkinnedClaim::SlotsExhausted);
        CHECK(budget.GetSlotCursor() == 1);
        CHECK(budget.GetPaletteCursor() == 4); // the palette was not consumed by the failure
    }
}

TEST_CASE("draw budget: palette bases are region-relative and contiguous")
{
    DrawBudget budget(16, 64);

    u32 slot = 0;
    u32 first = 0xFFFFFFFFU;
    u32 second = 0xFFFFFFFFU;
    u32 third = 0xFFFFFFFFU;
    CHECK(budget.TryClaimSkinnedDraw(3, slot, first) == SkinnedClaim::Granted);
    CHECK(budget.TryClaimSkinnedDraw(5, slot, second) == SkinnedClaim::Granted);
    CHECK(budget.TryClaimSkinnedDraw(2, slot, third) == SkinnedClaim::Granted);

    // Relative to the frame's palette region, not absolute: the caller folds the region base.
    CHECK(first == 0);
    CHECK(second == 3);
    CHECK(third == 8);
    CHECK(budget.GetPaletteCursor() == 10);
}

TEST_CASE("draw budget: a palette claim one matrix too large fails and consumes nothing")
{
    DrawBudget budget(16, 8);

    u32 slot = 0;
    u32 paletteBase = 0;
    CHECK(budget.TryClaimSkinnedDraw(5, slot, paletteBase) == SkinnedClaim::Granted);
    CHECK(paletteBase == 0);

    CHECK(budget.TryClaimSkinnedDraw(4, slot, paletteBase) == SkinnedClaim::PaletteExhausted);
    CHECK(budget.GetPaletteCursor() == 5);
    CHECK(budget.GetStats().PaletteDropped == 1);

    // The exact fit still succeeds after the oversized claim was refused whole.
    u32 tail = 0;
    CHECK(budget.TryClaimSkinnedDraw(3, slot, tail) == SkinnedClaim::Granted);
    CHECK(tail == 5);
    CHECK(budget.GetPaletteCursor() == 8);
}

TEST_CASE("draw budget: the slot and palette budgets are independent")
{
    SUBCASE("an exhausted palette leaves slots claimable")
    {
        DrawBudget budget(4, 2);

        u32 slot = 0;
        u32 paletteBase = 0;
        CHECK(budget.TryClaimSkinnedDraw(3, slot, paletteBase) == SkinnedClaim::PaletteExhausted);
        CHECK(budget.TryClaimSlot(slot));
        CHECK(slot == 0);
    }

    SUBCASE("exhausted slots leave the palette untouched")
    {
        DrawBudget budget(1, 64);

        u32 slot = 0;
        CHECK(budget.TryClaimSlot(slot));
        CHECK_FALSE(budget.TryClaimSlot(slot));
        CHECK(budget.GetPaletteCursor() == 0);
    }
}
