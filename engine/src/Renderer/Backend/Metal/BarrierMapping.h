#pragma once

// The device-free hazard decision mapped onto Metal 4's stage-to-stage barriers. Metal has no image
// layouts and tracks no resource, so a barrier is an execution and visibility dependency between
// stages alone: a decision whose two sides only read needs none, whatever layouts it names.
// MetalStage mirrors the bits of MTLStages, which the backend static-asserts, so this header stays
// plain C++ and unit-testable.

#include <Veng/Veng.h>

#include "../../BarrierDecision.h"

namespace Veng::Renderer::Metal
{
    /// @brief The Metal stages a barrier names, bit for bit MTLStages.
    enum class MetalStage : u64
    {
        None = 0,
        Vertex = 1ull << 0,
        Fragment = 1ull << 1,
        Dispatch = 1ull << 27,
        Blit = 1ull << 28,
        All = Vertex | Fragment | Dispatch | Blit,
    };
    VE_ENUM_FLAGS(MetalStage)

    /// @brief The stages a compute encoder runs, the only ones an intra-encoder barrier there names.
    inline constexpr MetalStage ComputeEncoderStages = MetalStage::Dispatch | MetalStage::Blit;

    /// @brief The stages a render encoder runs.
    inline constexpr MetalStage RenderEncoderStages = MetalStage::Vertex | MetalStage::Fragment;

    /// @brief The Metal stages an engine stage set covers on one side of a dependency.
    ///
    /// The top and bottom of the pipe follow Vulkan's reading: as the producing side the top waits
    /// for nothing and the bottom for everything; as the consuming side the top is everything and
    /// the bottom nothing. Host access synchronizes through fences, not barriers.
    /// @param stages    The engine stages.
    /// @param producing Whether @p stages is the side waited on.
    /// @return The Metal stages.
    [[nodiscard]] constexpr MetalStage ToMetalStages(const SyncStage stages, const bool producing)
    {
        MetalStage out = MetalStage::None;
        const auto has = [stages](const SyncStage stage)
        { return (stages & stage) != SyncStage::None; };
        if (has(SyncStage::AllCommands))
        {
            return MetalStage::All;
        }
        if (has(producing ? SyncStage::BottomOfPipe : SyncStage::TopOfPipe))
        {
            return MetalStage::All;
        }
        if (has(SyncStage::DrawIndirect))
        {
            out |= MetalStage::Vertex | MetalStage::Dispatch;
        }
        if (has(SyncStage::VertexShader))
        {
            out |= MetalStage::Vertex;
        }
        if (has(SyncStage::FragmentShader) || has(SyncStage::EarlyFragmentTests) ||
            has(SyncStage::LateFragmentTests) || has(SyncStage::ColorAttachmentOutput))
        {
            out |= MetalStage::Fragment;
        }
        if (has(SyncStage::ComputeShader))
        {
            out |= MetalStage::Dispatch;
        }
        // A transfer runs as a blit, or, for a colour clear, as a render pass's load and store.
        if (has(SyncStage::Transfer))
        {
            out |= MetalStage::Blit | MetalStage::Fragment;
        }
        return out;
    }

    /// @brief One stage-to-stage barrier.
    struct MetalBarrier
    {
        /// @brief The stages whose earlier work is waited on.
        MetalStage After = MetalStage::None;
        /// @brief The stages whose later work waits.
        MetalStage Before = MetalStage::None;

        /// @brief Whether the barrier orders anything.
        [[nodiscard]] bool IsNeeded() const
        {
            return After != MetalStage::None && Before != MetalStage::None;
        }
    };

    /// @brief Maps one dependency onto a Metal barrier.
    ///
    /// A read after a read orders only the stages the earlier reads did not run in: with no layouts
    /// there is nothing to transition, but a new stage was never ordered after the write those
    /// reads waited on, and waiting on the earlier readers' stages reaches it, as Vulkan's
    /// execution dependency chain does.
    /// @param srcStage  The producing stages.
    /// @param srcAccess The producing access.
    /// @param dstStage  The consuming stages.
    /// @param dstAccess The consuming access.
    /// @return The barrier, not needed when nothing is ordered.
    [[nodiscard]] inline MetalBarrier MapBarrier(const SyncStage srcStage,
                                                 const SyncAccess srcAccess,
                                                 const SyncStage dstStage,
                                                 const SyncAccess dstAccess)
    {
        const MetalStage after = ToMetalStages(srcStage, true);
        MetalStage before = ToMetalStages(dstStage, false);
        if (!IsWriteAccess(srcAccess) && !IsWriteAccess(dstAccess))
        {
            before = static_cast<MetalStage>(static_cast<u64>(before) & ~static_cast<u64>(after));
        }
        return {.After = after, .Before = before};
    }
}
