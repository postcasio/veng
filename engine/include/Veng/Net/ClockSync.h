#pragma once

#include <Veng/Veng.h>

// Veng/Net/ClockSync.h — the client tick-offset controller, pure and device-free.
//
// A displaying client stamps its input with its own sim tick, but the server consumes that input at
// the tick the input names. For the input to arrive at or just before its consumption front, the
// client must run its sim tick a little *ahead* of the server — far enough ahead to cover the trip
// time and the network's jitter, with a small safety margin. This file is the math that decides how
// far ahead (the target lead) and how to reach it (a bounded per-tick slew of the sim step, never a
// hard step, so no tick is doubled or skipped). The pure functions fold a smoothed RTT/jitter trace
// into the target and the slew; TickOffsetEstimator is the small stateful controller that smooths a
// raw RTT stream and holds the running estimate. No sockets, no scene, no time source — fully
// unit-testable over scripted traces, the DecideBarrier / ComputeCascades / ResolveActions idiom.

namespace Veng::Net
{
    /// @brief Tuning for the client tick-offset controller.
    ///
    /// The lead is expressed in ticks; TickRate converts the RTT/jitter seconds into ticks. MaxSlew
    /// bounds the per-tick stretch/shrink of the sim step so the client never doubles or skips a tick
    /// while chasing the target, and SlewGain is the proportional gain from a tick of offset error to
    /// that slew.
    struct TickSyncSettings
    {
        /// @brief Fixed simulation ticks per second — the seconds-to-ticks conversion.
        u32 TickRate = 60;
        /// @brief Fixed safety lead added beyond the jitter estimate, in ticks.
        f32 MarginTicks = 1.0f;
        /// @brief Bound on the per-tick step stretch/shrink, as a fraction (0.05 ⇒ ±5%).
        f32 MaxSlew = 0.05f;
        /// @brief Proportional gain mapping a tick of offset error to a step slew.
        f32 SlewGain = 0.2f;
    };

    /// @brief The lead, in ticks, the client should run ahead of the server.
    ///
    /// (RTT/2 + jitter)·TickRate + MarginTicks − feedbackTrim: the trip time one way, plus the jitter
    /// the buffer must absorb, plus the safety margin, less any closed-loop correction the server's
    /// consumed-input feedback supplies. Clamped at zero — the client never targets running behind.
    /// @param rttSeconds       Smoothed round-trip time in seconds.
    /// @param jitterSeconds    Smoothed RTT jitter (mean absolute deviation) in seconds.
    /// @param feedbackTrimTicks Closed-loop early/late correction in ticks (0 without feedback).
    /// @param settings         The controller tuning.
    /// @return The target lead in ticks, never negative.
    [[nodiscard]] VE_API f32 TargetTickOffset(f32 rttSeconds, f32 jitterSeconds,
                                              f32 feedbackTrimTicks,
                                              const TickSyncSettings& settings);

    /// @brief The bounded slew factor that drives the current lead toward the target.
    ///
    /// A multiplier on the next sim step: above one speeds the client up (grows the lead), below one
    /// slows it (shrinks the lead), clamped to [1 − MaxSlew, 1 + MaxSlew]. Because the factor stays
    /// within a few percent of one, the client advances by very nearly one tick per server tick — it
    /// never doubles a tick (factor ≥ 2) or stalls one (factor ≤ 0).
    /// @param offsetErrorTicks  targetOffset − currentOffset, in ticks (positive ⇒ run faster).
    /// @param settings          The controller tuning.
    /// @return The bounded step multiplier.
    [[nodiscard]] VE_API f32 SlewForOffsetError(f32 offsetErrorTicks,
                                                const TickSyncSettings& settings);

    /// @brief One controller step's inputs: the observed link state and current lead.
    struct TickOffsetInput
    {
        /// @brief Smoothed round-trip time in seconds.
        f32 RttSeconds = 0.0f;
        /// @brief Smoothed RTT jitter (mean absolute deviation) in seconds.
        f32 JitterSeconds = 0.0f;
        /// @brief The client's current lead over the server, clientTick − serverTick, in ticks.
        f32 CurrentOffsetTicks = 0.0f;
        /// @brief Closed-loop early/late correction from consumed-input feedback, in ticks.
        f32 FeedbackTrimTicks = 0.0f;
    };

    /// @brief One controller step's outputs: the target lead and the bounded slew that chases it.
    struct TickOffsetEstimate
    {
        /// @brief The lead in ticks the client should run ahead of the server.
        f32 TargetOffsetTicks = 0.0f;
        /// @brief The bounded step multiplier to apply next tick (see SlewForOffsetError).
        f32 SlewFactor = 1.0f;
    };

    /// @brief Folds the link state and current lead into the target lead and the bounded chasing slew.
    ///
    /// The whole per-update controller as one pure call: TargetTickOffset for the goal, then
    /// SlewForOffsetError over the gap to the current lead.
    /// @param input     The observed link state and current lead.
    /// @param settings  The controller tuning.
    /// @return The target lead and the bounded slew.
    [[nodiscard]] VE_API TickOffsetEstimate EstimateTickOffset(const TickOffsetInput& input,
                                                               const TickSyncSettings& settings);

    /// @brief The stateful client-side tick-offset controller: smooths a raw RTT stream, holds the estimate.
    ///
    /// Observe folds a raw RTT sample and the freshest (client, server) tick pair into a smoothed RTT
    /// and jitter (an exponential moving average of the RTT and of its absolute deviation), then runs
    /// EstimateTickOffset to produce the target lead and the bounded slew, both retained for the
    /// caller. The closed-loop feedback trim is set separately as the server's consumed-input signal
    /// arrives. Device-free and deterministic — it holds no clock and reads no socket, so it drives
    /// off injected samples exactly as it does off a live connection.
    class VE_API TickOffsetEstimator
    {
    public:
        /// @brief Constructs an estimator with the default tuning.
        TickOffsetEstimator() = default;

        /// @brief Constructs an estimator with the given tuning.
        /// @param settings  The controller tuning.
        explicit TickOffsetEstimator(const TickSyncSettings& settings) : m_Settings(settings) {}

        /// @brief Folds one observation into the smoothed estimate and returns the bounded slew.
        ///
        /// Updates the smoothed RTT and jitter from @p rttSeconds, computes the current lead from the
        /// tick pair, and recomputes the target lead and slew. The first observation seeds the
        /// smoothing (jitter starts at zero) rather than blending against an unset value.
        /// @param rttSeconds  The connection's smoothed round-trip time this update, in seconds.
        /// @param clientTick  The client's current sim tick.
        /// @param serverTick  The freshest server sim tick the client has heard.
        /// @return The bounded step multiplier to apply next tick.
        f32 Observe(f32 rttSeconds, u64 clientTick, u64 serverTick);

        /// @brief Sets the closed-loop early/late correction from the server's consumed-input feedback.
        /// @param trimTicks  The correction in ticks folded into the next target computation.
        void SetFeedbackTrim(f32 trimTicks) { m_FeedbackTrimTicks = trimTicks; }

        /// @brief The smoothed round-trip time in seconds (0 before the first observation).
        [[nodiscard]] f32 SmoothedRtt() const { return m_SmoothedRtt; }

        /// @brief The smoothed RTT jitter in seconds (0 before the second observation).
        [[nodiscard]] f32 SmoothedJitter() const { return m_SmoothedJitter; }

        /// @brief The most recently computed target lead in ticks.
        [[nodiscard]] f32 TargetOffset() const { return m_Estimate.TargetOffsetTicks; }

        /// @brief The most recently computed bounded slew factor.
        [[nodiscard]] f32 SlewFactor() const { return m_Estimate.SlewFactor; }

        /// @brief True once at least one observation has been folded in.
        [[nodiscard]] bool HasEstimate() const { return m_HasEstimate; }

    private:
        /// @brief The controller tuning.
        TickSyncSettings m_Settings;
        /// @brief Exponential moving average of the raw RTT, in seconds.
        f32 m_SmoothedRtt = 0.0f;
        /// @brief Exponential moving average of |RTT − smoothed RTT|, in seconds.
        f32 m_SmoothedJitter = 0.0f;
        /// @brief The last closed-loop feedback trim set, in ticks.
        f32 m_FeedbackTrimTicks = 0.0f;
        /// @brief The most recently computed target and slew.
        TickOffsetEstimate m_Estimate;
        /// @brief False until the first Observe seeds the smoothing.
        bool m_HasEstimate = false;
    };
}
