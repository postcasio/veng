#pragma once

// The device-free hazard decision: whether a use of an image subresource or a buffer must wait on
// an earlier one, and what each side of that wait names. It speaks the engine's own
// synchronization vocabulary (SyncStage, SyncAccess, ImageLayout), so it is unit-testable without
// a device and shared by every backend; a backend maps a decision onto its own barrier command.
// The render graph's schedule (RenderGraphSchedule.h) and the out-of-graph transitions both decide
// through it.

#include <Veng/Veng.h>
#include <Veng/Renderer/Types.h> // AccessKind, VE_ENUM_FLAGS
#include "ImageLayout.h"

namespace Veng::Renderer
{
    /// @brief Bitmask of pipeline stages a synchronization scope names.
    ///
    /// The stages a use runs in (its destination scope) or a prior use ran in (its source scope).
    /// None is an empty scope.
    enum class SyncStage : u32
    {
        /// @brief No stage.
        None = 0,
        /// @brief The point before any command's work begins.
        TopOfPipe = 1 << 0,
        /// @brief Reading indirect draw and dispatch arguments.
        DrawIndirect = 1 << 1,
        /// @brief Vertex shading.
        VertexShader = 1 << 2,
        /// @brief Fragment shading.
        FragmentShader = 1 << 3,
        /// @brief Depth/stencil tests before fragment shading.
        EarlyFragmentTests = 1 << 4,
        /// @brief Depth/stencil tests after fragment shading.
        LateFragmentTests = 1 << 5,
        /// @brief Colour attachment writes and blending.
        ColorAttachmentOutput = 1 << 6,
        /// @brief Compute shading.
        ComputeShader = 1 << 7,
        /// @brief Copies, blits, clears and resolves.
        Transfer = 1 << 8,
        /// @brief The point after every command's work ends.
        BottomOfPipe = 1 << 9,
        /// @brief Host reads and writes of mapped memory.
        Host = 1 << 10,
        /// @brief Every stage.
        AllCommands = 1 << 11,
    };

    /// @brief Bitmask of memory accesses a synchronization scope names.
    enum class SyncAccess : u32
    {
        /// @brief No access.
        None = 0,
        /// @brief Indirect draw and dispatch arguments read.
        IndirectCommandRead = 1 << 0,
        /// @brief A shader read (sampled, storage or uniform).
        ShaderRead = 1 << 1,
        /// @brief A shader write (storage).
        ShaderWrite = 1 << 2,
        /// @brief A colour attachment read (blending, load).
        ColorAttachmentRead = 1 << 3,
        /// @brief A colour attachment write.
        ColorAttachmentWrite = 1 << 4,
        /// @brief A depth/stencil attachment read (tests, load).
        DepthStencilAttachmentRead = 1 << 5,
        /// @brief A depth/stencil attachment write.
        DepthStencilAttachmentWrite = 1 << 6,
        /// @brief A copy or blit source read.
        TransferRead = 1 << 7,
        /// @brief A copy, blit or clear destination write.
        TransferWrite = 1 << 8,
        /// @brief A host read of mapped memory.
        HostRead = 1 << 9,
        /// @brief A host write of mapped memory.
        HostWrite = 1 << 10,
        /// @brief Any read.
        MemoryRead = 1 << 11,
        /// @brief Any write.
        MemoryWrite = 1 << 12,
    };

    VE_ENUM_FLAGS(SyncStage)
    VE_ENUM_FLAGS(SyncAccess)

    /// @brief Returns true when every bit of @p subset is set in @p set.
    [[nodiscard]] constexpr bool Covers(const SyncStage set, const SyncStage subset)
    {
        return (set & subset) == subset;
    }

    /// @brief Returns true when every bit of @p subset is set in @p set.
    [[nodiscard]] constexpr bool Covers(const SyncAccess set, const SyncAccess subset)
    {
        return (set & subset) == subset;
    }

    /// @brief The queue-family index meaning "no ownership transfer" (graphics-produced).
    inline constexpr u32 QueueFamilyIgnored = ~0u;

    /// @brief The tracked (or desired) state of an image subresource: the layout it is in plus
    /// the stage/access scope that last touched — or will touch — it.
    ///
    /// Shared by the per-subresource tracked-state store and the declared-use table (they are the
    /// same triple). A buffer scope uses Stage and Access only; its Layout stays Undefined.
    struct SubresourceState
    {
        /// @brief Image layout of the subresource.
        ImageLayout Layout = ImageLayout::Undefined;
        /// @brief Pipeline stage(s) that last accessed the subresource.
        SyncStage Stage = SyncStage::None;
        /// @brief Memory access(es) of the last access.
        SyncAccess Access = SyncAccess::None;

        /// @brief The queue family that last produced this subresource.
        ///
        /// A resource uploaded on the transfer queue needs a queue-family ownership transfer
        /// before the graphics queue may use it; the acquire half of that transfer keys off this
        /// field. QueueFamilyIgnored means graphics-produced — the default until an upload marks
        /// otherwise — so an ordinary graphics-only resource never triggers the acquire branch.
        u32 ProducingFamily = QueueFamilyIgnored;

        [[nodiscard]] bool operator==(const SubresourceState&) const = default;
    };

    /// @brief Outcome of comparing a subresource's current state against a desired use.
    struct BarrierDecision
    {
        /// @brief Whether a barrier must be recorded.
        bool NeedsBarrier;
        /// @brief State to record after this use (always valid).
        SubresourceState NewState;
        /// @brief Barrier source — valid only when NeedsBarrier.
        SubresourceState Src;
        /// @brief Barrier destination — valid only when NeedsBarrier.
        SubresourceState Dst;

        /// @brief Queue-family indices for the barrier's ownership transfer.
        ///
        /// Both are QueueFamilyIgnored unless this is the acquire half of a transfer→graphics
        /// handoff, in which case SrcQueueFamilyIndex is the transfer family and
        /// DstQueueFamilyIndex is the graphics family. Equal families collapse both back to
        /// QueueFamilyIgnored — an ordinary same-queue transition.
        u32 SrcQueueFamilyIndex = QueueFamilyIgnored;
        /// @brief Destination queue family for the ownership transfer (or QueueFamilyIgnored).
        u32 DstQueueFamilyIndex = QueueFamilyIgnored;
    };

    /// @brief Returns true if any of the access bits is a write (vs a pure read).
    [[nodiscard]] VE_API bool IsWriteAccess(SyncAccess access);

    /// @brief Decides whether a barrier is needed to transition a subresource from @p current
    /// state to the desired layout/stage/access.
    ///
    /// - Read-after-read in the same layout, inside the tracked read scope (every desired stage
    ///   and access bit already in @p current): NeedsBarrier=false.
    /// - Read-after-read in the same layout that names a stage or access outside the tracked read
    ///   scope: NeedsBarrier=true, a layout-preserving barrier from the earlier readers' stages (no
    ///   source access) to the desired stage/access. A barrier whose destination was the earlier
    ///   readers does not order the new one after the last write; this one chains after that
    ///   barrier instead.
    /// - In both read-after-read cases NewState keeps the layout and widens the read scope
    ///   (Stage/Access OR'd with the desired), so a later write waits on every prior read.
    /// - Hazard (layout change, or either side writes): NeedsBarrier=true; the barrier goes current
    ///   → desired, and the new tracked state is the desired.
    ///
    /// @p transferFamily and @p graphicsFamily give the rule its queue dimension. When the
    /// subresource was produced on transferFamily and the two families differ, the resulting
    /// barrier is the acquire half of a queue-family ownership transfer. When the families match
    /// (single-queue collapse) both queue indices stay QueueFamilyIgnored. The consumed subresource
    /// is graphics-produced thereafter, so a later use never re-acquires.
    [[nodiscard]] VE_API BarrierDecision DecideBarrier(const SubresourceState& current,
                                                       ImageLayout newLayout, SyncStage dstStage,
                                                       SyncAccess dstAccess, u32 transferFamily,
                                                       u32 graphicsFamily);

    /// @brief Returns the layout/stage/access scope required by the given access kind.
    ///
    /// Pure mapping used by the render graph and CommandBuffer::PrepareForAccess, testable
    /// alongside DecideBarrier.
    [[nodiscard]] VE_API SubresourceState ScopeFor(AccessKind kind);

    /// @brief Returns the destination scope of a transition to @p layout with no declared use.
    ///
    /// The out-of-graph transitions (upload, download, mip generation, present) name only the
    /// layout they need; this derives the stage and access that layout implies.
    [[nodiscard]] VE_API SubresourceState ScopeForLayout(ImageLayout layout);
}
