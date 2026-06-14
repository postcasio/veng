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
    // The tracked (or desired) state of an image subresource: the layout it is in
    // plus the pipeline stage/access scope that last touched — or will touch —
    // it. Shared by the per-subresource tracked-state store and the declared-use
    // table below (they are the same triple).
    struct SubresourceState
    {
        vk::ImageLayout Layout;
        vk::PipelineStageFlags Stage;
        vk::AccessFlags Access;

        // The queue family that last produced this subresource. A resource
        // uploaded on the transfer queue needs a queue-family ownership transfer
        // before the graphics queue may use it; the acquire half of that transfer
        // keys off this field. The value VK_QUEUE_FAMILY_IGNORED means
        // "graphics-produced" — the default until an upload marks otherwise — so
        // an ordinary graphics-only resource never triggers the acquire branch.
        u32 ProducingFamily = VK_QUEUE_FAMILY_IGNORED;
    };

    // Outcome of comparing a subresource's current state against a desired use.
    struct BarrierDecision
    {
        bool NeedsBarrier;
        SubresourceState NewState; // the state to record after this use (always valid)
        SubresourceState Src;      // barrier source — valid only when NeedsBarrier
        SubresourceState Dst;      // barrier destination — valid only when NeedsBarrier

        // Queue-family ownership transfer for the barrier. Both are
        // VK_QUEUE_FAMILY_IGNORED unless this is the acquire half of a
        // transfer→graphics handoff, in which case Src is the transfer family and
        // Dst is the graphics family. The two are always paired: the release half
        // (recorded by the upload) uses the same src/dst, and equal families
        // collapse both back to IGNORED — an ordinary same-queue transition.
        u32 SrcQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
        u32 DstQueueFamilyIndex = VK_QUEUE_FAMILY_IGNORED;
    };

    // True if any of the access bits is a write (vs a pure read).
    [[nodiscard]] bool IsWriteAccess(vk::AccessFlags access);

    // The hazard rule (behaviour-preserving extract of TransitionImage):
    //  - No hazard (same layout, read-after-read): NeedsBarrier=false; NewState
    //    keeps the current layout and widens the read scope (Stage/Access OR'd
    //    with the desired) so a later write waits on every prior read.
    //  - Hazard (layout change, or either side writes): NeedsBarrier=true; the
    //    barrier goes current -> desired, and the new tracked state is the desired.
    //
    // transferFamily/graphicsFamily give the rule its queue dimension. When the
    // current subresource was produced on transferFamily and the two families
    // differ, the resulting barrier is the acquire half of a queue-family
    // ownership transfer (SrcQueueFamilyIndex=transfer, DstQueueFamilyIndex=
    // graphics). When the families match — the single-queue collapse — both stay
    // VK_QUEUE_FAMILY_IGNORED and the barrier is the ordinary same-queue
    // transition. The consumed subresource is graphics-produced thereafter, so a
    // later use never re-acquires.
    [[nodiscard]] BarrierDecision DecideBarrier(const SubresourceState& current,
                                                vk::ImageLayout newLayout,
                                                vk::PipelineStageFlags dstStage,
                                                vk::AccessFlags dstAccess,
                                                u32 transferFamily,
                                                u32 graphicsFamily);

    // The declared-use table: each access kind resolves to the layout the image
    // must be in plus the stage/access scope that uses it. Pure; lives here so it
    // is testable alongside DecideBarrier.
    [[nodiscard]] SubresourceState ScopeFor(AccessKind kind);
}
