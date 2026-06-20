#pragma once

// Pure, device-free barrier-decision logic, kept separate from Barrier.cpp so
// it can be unit-tested without a GPU. Backend header — it uses vk:: types —
// so only backend .cpp files and tests include it. TransitionImage reads
// tracked state, calls DecideBarrier, emits a barrier iff NeedsBarrier, and
// records NewState.

#include <Veng/Renderer/Backend/Vulkan.h>
#include <Veng/Renderer/Types.h> // AccessKind (for ScopeFor)

namespace Veng::Renderer::Backend
{
    /// @brief The tracked (or desired) state of an image subresource: the layout
    /// it is in plus the pipeline stage/access scope that last touched — or will
    /// touch — it.
    ///
    /// Shared by the per-subresource tracked-state store and the declared-use
    /// table (they are the same triple).
    struct SubresourceState
    {
        /// @brief Image layout of the subresource.
        vk::ImageLayout Layout;
        /// @brief Pipeline stage(s) that last accessed the subresource.
        vk::PipelineStageFlags Stage;
        /// @brief Access flags for the last access.
        vk::AccessFlags Access;

        /// @brief The queue family that last produced this subresource.
        ///
        /// A resource uploaded on the transfer queue needs a queue-family ownership
        /// transfer before the graphics queue may use it; the acquire half of that
        /// transfer keys off this field. VK_QUEUE_FAMILY_IGNORED means
        /// graphics-produced — the default until an upload marks otherwise — so an
        /// ordinary graphics-only resource never triggers the acquire branch.
        u32 ProducingFamily = VK_QUEUE_FAMILY_IGNORED;
    };

    /// @brief Outcome of comparing a subresource's current state against a desired use.
    struct BarrierDecision
    {
        /// @brief Whether a pipeline barrier must be emitted.
        bool NeedsBarrier;
        /// @brief State to record after this use (always valid).
        SubresourceState NewState;
        /// @brief Barrier source — valid only when NeedsBarrier.
        SubresourceState Src;
        /// @brief Barrier destination — valid only when NeedsBarrier.
        SubresourceState Dst;

        /// @brief Queue-family indices for the barrier's ownership transfer.
        ///
        /// Both are VK_QUEUE_FAMILY_IGNORED unless this is the acquire half of a
        /// transfer→graphics handoff, in which case SrcQueueFamilyIndex is the
        /// transfer family and DstQueueFamilyIndex is the graphics family. Equal
        /// families collapse both back to IGNORED — an ordinary same-queue transition.
        u32 SrcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        /// @brief Destination queue family for the ownership transfer (or VK_QUEUE_FAMILY_IGNORED).
        u32 DstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    };

    /// @brief Returns true if any of the access bits is a write (vs a pure read).
    [[nodiscard]] bool IsWriteAccess(vk::AccessFlags access);

    /// @brief Decides whether a barrier is needed to transition a subresource
    /// from @p current state to the desired layout/stage/access.
    ///
    /// - No hazard (same layout, read-after-read): NeedsBarrier=false; NewState
    ///   keeps the current layout and widens the read scope (Stage/Access OR'd with
    ///   the desired) so a later write waits on every prior read.
    /// - Hazard (layout change, or either side writes): NeedsBarrier=true; the
    ///   barrier goes current → desired, and the new tracked state is the desired.
    ///
    /// @p transferFamily and @p graphicsFamily give the rule its queue dimension.
    /// When the subresource was produced on transferFamily and the two families
    /// differ, the resulting barrier is the acquire half of a queue-family ownership
    /// transfer. When the families match (single-queue collapse) both queue indices
    /// stay VK_QUEUE_FAMILY_IGNORED. The consumed subresource is graphics-produced
    /// thereafter, so a later use never re-acquires.
    [[nodiscard]] BarrierDecision DecideBarrier(const SubresourceState& current,
                                                vk::ImageLayout newLayout,
                                                vk::PipelineStageFlags dstStage,
                                                vk::AccessFlags dstAccess, u32 transferFamily,
                                                u32 graphicsFamily);

    /// @brief Returns the layout/stage/access scope required by the given access kind.
    ///
    /// Pure mapping used by TransitionImage and testable alongside DecideBarrier.
    [[nodiscard]] SubresourceState ScopeFor(AccessKind kind);
}
