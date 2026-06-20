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
    /// @brief A transient's live range over the linear pass order: the index of
    /// the pass that first writes it through the index of the pass that last reads it.
    ///
    /// A transient written but never read (e.g. a DontCare depth buffer) has
    /// LastUse == FirstUse.
    struct TransientLifetime
    {
        /// @brief Pass index of first write.
        u32 FirstUse;
        /// @brief Pass index of last read.
        u32 LastUse;
    };

    /// @brief The size class that determines whether two transients may share a
    /// physical allocation.
    ///
    /// A transient may reuse another's backing only if its key matches AND their
    /// lifetimes do not overlap.
    struct AllocationKey
    {
        /// @brief Pixel format of the transient image.
        Format Format;
        /// @brief Pixel dimensions of the transient image.
        uvec2 Extent;
        /// @brief Usage flags the backing allocation must support.
        ImageUsage Usage;
        [[nodiscard]] bool operator==(const AllocationKey&) const = default;
    };

    /// @brief Assigns each transient to a physical allocation slot.
    ///
    /// Transients with non-overlapping lifetimes and equal AllocationKey share a
    /// slot; all others get their own. Greedy over FirstUse order — correct and
    /// minimal on a linear graph.
    /// @param lifetimes  Per-transient live ranges.
    /// @param keys       Per-transient size-class keys (parallel to lifetimes).
    /// @return One slot index per input transient (parallel array, same indexing).
    /// @pre lifetimes and keys must have equal length.
    [[nodiscard]] vector<u32> AssignTransientSlots(const vector<TransientLifetime>& lifetimes,
                                                   const vector<AllocationKey>& keys);
}
