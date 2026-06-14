#pragma once

// Pure, device-free transient-allocation logic, sited beside BarrierDecision.h
// as the other graph-compile rule that reasons about declarations rather than
// the driver. It is vk-free — it works in pass indices and allocation
// compatibility, not layouts — but lives in Backend/ to group with the compile
// internals and stay off the public surface. Only the compiler and its unit
// tests include it.

#include <Veng/Veng.h>
#include <Veng/Renderer/Types.h> // Format, ImageUsage

namespace Veng::Renderer::Backend
{
    // A transient's live range over the linear pass order: the index of the pass
    // that first writes it through the index of the pass that last reads it. A
    // transient written but never read (e.g. a DontCare depth buffer) has
    // LastUse == FirstUse.
    struct TransientLifetime
    {
        u32 FirstUse; // pass index of first write
        u32 LastUse;  // pass index of last read
    };

    // What makes two transients share an allocation: the same size class. A
    // transient may reuse another's backing only if its key matches AND their
    // lifetimes do not overlap.
    struct AllocationKey
    {
        Format Format;
        uvec2 Extent;
        ImageUsage Usage;
        [[nodiscard]] bool operator==(const AllocationKey&) const = default;
    };

    // Assign each transient to a physical allocation slot. Transients with
    // non-overlapping lifetimes and equal AllocationKey share a slot; everything
    // else gets its own. Greedy over FirstUse order — correct and minimal on a
    // linear graph. Returns one slot index per input transient (parallel array,
    // indexed the same as lifetimes/keys). lifetimes and keys must have equal
    // length.
    [[nodiscard]] vector<u32> AssignTransientSlots(const vector<TransientLifetime>& lifetimes,
                                                   const vector<AllocationKey>& keys);
}
