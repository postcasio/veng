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
    };

    // Outcome of comparing a subresource's current state against a desired use.
    struct BarrierDecision
    {
        bool NeedsBarrier;
        SubresourceState NewState; // the state to record after this use (always valid)
        SubresourceState Src;      // barrier source — valid only when NeedsBarrier
        SubresourceState Dst;      // barrier destination — valid only when NeedsBarrier
    };

    // True if any of the access bits is a write (vs a pure read).
    [[nodiscard]] bool IsWriteAccess(vk::AccessFlags access);

    // The hazard rule (behaviour-preserving extract of TransitionImage):
    //  - No hazard (same layout, read-after-read): NeedsBarrier=false; NewState
    //    keeps the current layout and widens the read scope (Stage/Access OR'd
    //    with the desired) so a later write waits on every prior read.
    //  - Hazard (layout change, or either side writes): NeedsBarrier=true; the
    //    barrier goes current -> desired, and the new tracked state is the desired.
    [[nodiscard]] BarrierDecision DecideBarrier(const SubresourceState& current,
                                                vk::ImageLayout newLayout,
                                                vk::PipelineStageFlags dstStage,
                                                vk::AccessFlags dstAccess);

    // The declared-use table: each access kind resolves to the layout the image
    // must be in plus the stage/access scope that uses it. Pure; lives here so it
    // is testable alongside DecideBarrier.
    [[nodiscard]] SubresourceState ScopeFor(AccessKind kind);
}
