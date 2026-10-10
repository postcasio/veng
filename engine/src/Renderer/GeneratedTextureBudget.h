#pragma once

// The generated-texture service's adaptive budget, separated from the GPU half so the policy — the
// frame period, the headroom, the learned cost of a tick, the damping and the fallbacks — is a pure
// function of synthetic measurements and is unit-testable with no ICD (the GeneratedTextureQueue
// precedent). It reads no clock and no device: the service hands it each frame's readings.

#include <span>

#include <Veng/Veng.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/GeneratedTextureService.h>

namespace Veng::Renderer
{
    /// @brief What one pump knows about the frame it is about to spend.
    struct GeneratedTextureBudgetInput
    {
        /// @brief How the frame is paced and whether its clock is driven.
        GeneratedTextureFrameHint Hint;
        /// @brief Whether the device can timestamp GPU work at all.
        bool TimingSupported = false;
        /// @brief The last measured frame's whole GPU time in milliseconds; zero before the first.
        f32 GpuFrameMs = 0.0f;
        /// @brief The pump scope's GPU time within that frame, in milliseconds.
        f32 PumpMs = 0.0f;
        /// @brief Whether any job has a tick or a restore the pump could spend on.
        bool HasWork = false;
    };

    /// @brief The adaptive per-frame cost budget the generated-texture pump spends.
    ///
    /// Each frame Resolve turns the frame's period and its measured GPU idle time into a cost
    /// budget: the target is the idle time's share, clamped to an absolute ceiling, converted
    /// through a learned milliseconds-per-cost that Observe folds in from each measured pump. The
    /// result rises a fraction of the gap a frame and falls to the target at once, and never drops
    /// below the floor.
    class GeneratedTextureBudgetController
    {
    public:
        /// @brief The longest frame period the policy takes as a frame's, in milliseconds.
        ///
        /// A hitch — a load, a breakpoint — measures as one very long frame, which would otherwise
        /// read as a great deal of idle time.
        static constexpr f32 MaxPeriodMs = 100.0f;

        /// @brief The weight a new frame delta takes in the smoothed period.
        static constexpr f32 PeriodSmoothing = 0.1f;

        /// @brief The weight a new sample takes in the learned milliseconds-per-cost.
        static constexpr f32 CostSmoothing = 0.25f;

        /// @brief The fraction of the gap to its target the budget rises by in one frame.
        static constexpr f32 RiseRate = 0.125f;

        /// @brief The most a device without GPU timing scales the floor by.
        static constexpr f32 MaxFallbackScale = 4.0f;

        /// @brief The largest budget the controller resolves, far above any frame's worth of work.
        static constexpr u32 MaxBudget = 1u << 20;

        /// @brief Constructs the controller at its floor.
        /// @param floor The budget it never resolves below; at least one.
        explicit GeneratedTextureBudgetController(u32 floor);

        /// @brief Folds one measured pump into the learned milliseconds-per-cost.
        ///
        /// A frame whose pump spent nothing, or whose pump scope went unmeasured, teaches nothing
        /// and is ignored.
        /// @param pumpMs     The pump scope's GPU time, in milliseconds.
        /// @param costSpent  The cost that pump spent.
        void Observe(f32 pumpMs, u64 costSpent);

        /// @brief Resolves the budget for the frame the input describes.
        /// @param input     The frame's readings.
        /// @param settings  The policy's knobs.
        /// @return The summed tick cost the pump may spend this frame.
        [[nodiscard]] u32 Resolve(const GeneratedTextureBudgetInput& input,
                                  const GeneratedTextureBudgetSettings& settings);

        /// @brief The frame period the last Resolve used, in milliseconds; zero when none was known.
        [[nodiscard]] f32 GetPeriodMs() const { return m_PeriodMs; }

        /// @brief The learned GPU milliseconds one unit of cost takes; zero before the first sample.
        [[nodiscard]] f32 GetMillisecondsPerCost() const { return m_MsPerCost; }

        /// @brief The pump time the last Resolve aimed for, in milliseconds; zero off the timed path.
        [[nodiscard]] f32 GetTargetMs() const { return m_TargetMs; }

        /// @brief The budget the last Resolve returned.
        [[nodiscard]] u32 GetBudget() const { return m_Budget; }

    private:
        /// @brief The frame period from the hint, smoothing a measured delta; zero when unknown.
        [[nodiscard]] f32 ResolvePeriodMs(const GeneratedTextureFrameHint& hint);

        /// @brief The budget a device without GPU timing spends: the floor scaled by the period.
        [[nodiscard]] u32 ResolveFallback(f32 periodMs,
                                          const GeneratedTextureBudgetSettings& settings);

        /// @brief The budget never resolved below.
        u32 m_Floor;
        /// @brief The budget the last Resolve returned.
        u32 m_Budget;
        /// @brief The smoothed measured frame delta in milliseconds; zero before the first.
        f32 m_SmoothedDeltaMs = 0.0f;
        /// @brief The period the last Resolve used.
        f32 m_PeriodMs = 0.0f;
        /// @brief The learned milliseconds-per-cost; zero before the first sample.
        f32 m_MsPerCost = 0.0f;
        /// @brief The target the last timed Resolve aimed for.
        f32 m_TargetMs = 0.0f;
        /// @brief The fractional cost the fallback carries into the next frame.
        f32 m_FallbackCarry = 0.0f;
    };

    /// @brief The GPU time of the first top-level scope of a name in a frame's timings.
    /// @param timings  A frame's per-pass timings (Context::GetLastGpuPassTimings).
    /// @param name     The scope's name.
    /// @return Its milliseconds, or zero when the frame recorded no such top-level scope.
    [[nodiscard]] f32 FindTopLevelGpuScopeMs(std::span<const Context::GpuPassTiming> timings,
                                             string_view name);
}
