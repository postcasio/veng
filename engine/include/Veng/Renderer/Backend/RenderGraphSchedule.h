#pragma once

// Pure, device-free render-graph schedule derivation, split from RenderGraph::Compile
// so the barrier/transition/attachment schedule a compiled graph replays can be
// unit-tested without a GPU — mirroring how BarrierDecision.h carries the per-edge
// hazard rule. Backend header — it uses vk:: types — so only backend .cpp files and
// tests include it. Compile() supplies the device-allocated transient backing; this
// function derives only the per-pass schedule and runs the structural validation.

#include <Veng/Renderer/Backend/BarrierDecision.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/Types.h>

namespace Veng::Renderer::Backend
{
    /// @brief A resource-table slot as seen by schedule derivation.
    ///
    /// The device-free projection of RenderGraph's resource table: whether the slot
    /// is imported or graph-owned, its image/buffer axis, the usage flags the
    /// declared accesses are validated against, and the name used in diagnostics.
    struct ScheduleResource
    {
        /// @brief True for an imported (externally-owned) resource.
        bool IsImport = false;
        /// @brief True for a buffer resource; false for an image resource.
        bool IsBuffer = false;
        /// @brief Debug name, used in validation messages.
        string Name;
        /// @brief Declared usage of a transient image, validated against its accesses.
        ImageUsage ImageUsage{};
        /// @brief Declared usage of a transient buffer, validated against its accesses.
        BufferUsage BufferUsage{};
    };

    /// @brief One pass's declared accesses as seen by schedule derivation.
    struct SchedulePass
    {
        /// @brief Debug name, used in validation messages.
        string_view Name;
        /// @brief The pass's declared resource accesses, in declaration order.
        std::span<const RenderGraph::Access> Accesses;
    };

    /// @brief A baked image transition: the slot and the destination scope to transition to.
    ///
    /// Only the destination is derived here; the source state and subresource range
    /// come from the resolved view's live tracked state at replay.
    struct ScheduledTransition
    {
        /// @brief Resource-table slot the transition applies to.
        u32 Slot;
        /// @brief Destination layout/stage/access scope derived from the access kind.
        SubresourceState Dst;
    };

    /// @brief A baked buffer barrier: both scopes are derived here.
    ///
    /// A buffer carries no runtime tracked state, so the source (the prior pass's
    /// declared scope on this slot) and the destination (this pass's declared scope)
    /// both bake at derivation. Emitted only on a hazard — the prior access wrote or
    /// this access writes.
    struct ScheduledBufferBarrier
    {
        /// @brief Resource-table slot the barrier applies to.
        u32 Slot;
        /// @brief Source pipeline stage(s) — the prior access's scope.
        vk::PipelineStageFlags SrcStage;
        /// @brief Source access flags — the prior access's scope.
        vk::AccessFlags SrcAccess;
        /// @brief Destination pipeline stage(s) — this access's scope.
        vk::PipelineStageFlags DstStage;
        /// @brief Destination access flags — this access's scope.
        vk::AccessFlags DstAccess;
    };

    /// @brief A baked graphics attachment: the slot plus its load/store/clear declaration.
    struct ScheduledAttachment
    {
        /// @brief Resource-table slot bound to this attachment.
        u32 Slot;
        /// @brief True for the depth attachment; false for a color attachment.
        bool IsDepth;
        /// @brief Attachment load operation.
        LoadOp Load;
        /// @brief Attachment store operation.
        StoreOp Store;
        /// @brief Clear value used when Load is Clear.
        ClearValue Clear;
    };

    /// @brief The derived schedule for one pass: its transitions, buffer barriers, and attachments.
    struct ScheduledPass
    {
        /// @brief Image layout/scope transitions to emit before the pass.
        vector<ScheduledTransition> Transitions;
        /// @brief Buffer barriers to emit before the pass.
        vector<ScheduledBufferBarrier> BufferBarriers;
        /// @brief Color/depth attachments the pass renders into (graphics passes only).
        vector<ScheduledAttachment> Attachments;
    };

    /// @brief Derives the per-pass barrier/transition/attachment schedule from declared accesses.
    ///
    /// The device-free core of RenderGraph::Compile: it scans passes in declaration
    /// order, tracks each slot's write history and (for buffers) its last declared
    /// scope, and produces the schedule a CompiledGraph replays. The same per-edge
    /// rules BarrierDecision.h documents drive it: an image access bakes a transition
    /// to its scope; a buffer access bakes a barrier only on a hazard, OR-accumulating
    /// the scope across reads so a later write waits on every prior read.
    ///
    /// It also runs the structural validation Compile relies on, asserting fatally on:
    /// an access whose buffer/image axis disagrees with its resource; a transient read
    /// before any pass writes it; and a transient whose usage flags lack what an
    /// attachment/sample/indirect access requires. Imports are exempt from the
    /// read-before-write and usage checks (their state and usage are external).
    ///
    /// @param resources  The resource table, indexed by ResourceId::Index.
    /// @param passes      The passes, in declaration order.
    /// @return One ScheduledPass per input pass, in the same order.
    /// @pre Every access's ResourceId::Index is in range of @p resources.
    [[nodiscard]] vector<ScheduledPass>
    DeriveRenderGraphSchedule(std::span<const ScheduleResource> resources,
                              std::span<const SchedulePass> passes);
}
