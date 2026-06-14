# Plan 03 — Transient aliasing + pure rule

**Goal:** let non-overlapping transients **share backing memory.** With a fixed,
linear pass order each transient's live range is a trivial pass-index interval (first
write … last read); two transients whose intervals do not overlap can resolve to the
same allocation. Extract the assignment as a **pure, device-free rule**, unit-test it
without a GPU, then wire it into the compiler — exactly the `DecideBarrier`/`ScopeFor`
pattern.

## Why this is its own plan

Plans 01–02 deliberately gave each transient its own allocation — the schedule cache
and the resource-model split are the load-bearing change, and aliasing is a memory
optimization on top, safe to add once the lifetime analysis is trusted. Isolating it
here keeps the risky compile/replay flip (02) free of aliasing bugs, and the analysis
is pure logic with no Vulkan, so it is fully unit-testable and a clean `model: sonnet`
delegation once the interface is fixed.

## The pure rule — new backend header

A new `engine/include/Veng/Renderer/Backend/TransientAllocation.h`, sited beside
`BarrierDecision.h` as the device-free graph-compile logic. It is vk-free (it reasons
about pass indices and allocation compatibility, not layouts), so it could be a plain
header; keeping it in `Backend/` groups it with the other compile internals and out of
the public surface.

```cpp
namespace Veng::Renderer::Backend
{
    // A transient's live range over the linear pass order: the index of the pass
    // that first writes it through the index of the pass that last reads it.
    struct TransientLifetime
    {
        u32 FirstUse;   // pass index of first write
        u32 LastUse;    // pass index of last read
    };

    // What makes two transients share an allocation: same size class. A transient
    // can reuse another's backing only if its allocation is compatible AND their
    // lifetimes do not overlap.
    struct AllocationKey
    {
        Format Format;
        uvec2  Extent;
        ImageUsage Usage;
        [[nodiscard]] bool operator==(const AllocationKey&) const = default;
    };

    // Assign each transient to a physical allocation slot. Transients with
    // non-overlapping lifetimes and equal AllocationKey share a slot; everything
    // else gets its own. Greedy over pass order — correct and near-free on a linear
    // graph. Returns one slot index per input transient (parallel array).
    [[nodiscard]] vector<u32> AssignTransientSlots(
        const vector<TransientLifetime>& lifetimes,
        const vector<AllocationKey>& keys);
}
```

Two lifetimes overlap iff `a.FirstUse <= b.LastUse && b.FirstUse <= a.LastUse`. A
transient written but never read (e.g. a DontCare depth buffer) has `LastUse ==
FirstUse`. The implementation walks transients in `FirstUse` order, reusing the first
freed, key-compatible slot or opening a new one.

## Unit tests — new `tests/unit/transient_allocation.cpp` (label `unit`, no GPU)

Add to the `veng_unit` source list in the top-level `CMakeLists.txt`. Pin the rule:

- **Disjoint, same key → shared slot.** Two transients with non-overlapping intervals
  and equal `AllocationKey` get the same slot index.
- **Overlapping → distinct slots.** Intervals that touch or nest never share.
- **Key mismatch blocks sharing.** Disjoint lifetimes but different format/extent/usage
  → distinct slots.
- **Chain reuse.** Three transients A(0–1), B(2–3), C(4–5), same key → all collapse to
  one slot; A(0–3), B(1–2) overlap → two slots.
- **Write-only lifetime.** `FirstUse == LastUse` is handled (a never-read transient).
- **Slot count is minimal** for the greedy linear cases above.

## Wiring into compile — `engine/src/Renderer/RenderGraph.cpp`

In `Compile()` (plan 02), before allocating transients: compute each transient's
`TransientLifetime` from the pass accesses (scan passes in order, record first write
and last read per transient slot) and its `AllocationKey` from its `TransientDesc`,
call `AssignTransientSlots`, then allocate **one `Image` per distinct slot** and point
each transient's resolved view at its slot's image. Per-frame replay is unchanged —
`PassContext::Resolved` returns the (possibly shared) slot image's view.

Aliased transients sharing memory means a later pass's write reuses an earlier
transient's storage; the **barrier schedule already serializes them** (the reuse only
happens across non-overlapping lifetimes, and the write transition waits on the prior
content's last read via the existing `DecideBarrier` hazard rule). No extra barrier is
needed beyond what the schedule already emits — note this as the correctness argument.

## GPU test — new case in the `veng_gpu` suite

The sample has only one transient (depth), so aliasing is not exercised there. Add a
`tests/gpu/` case (per-case `Context` fixture, `gpu` label, skips with no ICD) that
builds a compiled graph with **≥2 non-overlapping transients of the same key** plus a
read-back, asserting: the two transients resolve to the **same** underlying image
(slot sharing happened), and the final contents are correct (the schedule serialized
the reuse — no corruption). A negative case with overlapping lifetimes asserts
**distinct** images.

## Acceptance

- Clean build; `ctest -L unit` green (new `transient_allocation` cases); `ctest -L gpu`
  green (new aliasing case, or skipped with no ICD).
- `include_hygiene` unaffected (the rule is backend-only).
- Full `ctest` green; smoke binary writes a correct-sized PPM (the sample's single
  transient is unchanged by aliasing).
- `ctest --test-dir build-debug -L validation` green — aliased images are created,
  transitioned, and reused validation-clean; allowlist stays empty.
