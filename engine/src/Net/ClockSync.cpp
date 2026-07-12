#include <Veng/Net/ClockSync.h>

#include <algorithm>

namespace Veng::Net
{
    namespace
    {
        // Exponential-moving-average weight for the raw RTT and its deviation. Matching the
        // Connection's own RTT smoothing keeps the two filters on the same timescale.
        constexpr f32 RttSmoothing = 0.1f;
        constexpr f32 JitterSmoothing = 0.1f;
    }

    f32 TargetTickOffset(const f32 rttSeconds, const f32 jitterSeconds, const f32 feedbackTrimTicks,
                         const TickSyncSettings& settings)
    {
        const f32 tickRate = static_cast<f32>(settings.TickRate);
        // The offset the controller steers is measured against the last snapshot's server tick, which
        // lags the server's live tick by the downstream latency (~RTT/2). So the lead must cover the
        // whole round trip — the upstream trip the input still has to make (~RTT/2) plus that
        // downstream staleness (~RTT/2) — not just the one-way trip, or the client ends up level with
        // the live server and its input arrives after the consume front (chronic jitter-buffer
        // underrun). Plus the jitter the buffer absorbs and the safety margin.
        const f32 leadSeconds = rttSeconds + jitterSeconds;
        const f32 target = leadSeconds * tickRate + settings.MarginTicks - feedbackTrimTicks;
        return std::max(target, 0.0f);
    }

    f32 SlewForOffsetError(const f32 offsetErrorTicks, const TickSyncSettings& settings)
    {
        const f32 slew = 1.0f + settings.SlewGain * offsetErrorTicks;
        return std::clamp(slew, 1.0f - settings.MaxSlew, 1.0f + settings.MaxSlew);
    }

    TickOffsetEstimate EstimateTickOffset(const TickOffsetInput& input,
                                          const TickSyncSettings& settings)
    {
        const f32 target = TargetTickOffset(input.RttSeconds, input.JitterSeconds,
                                            input.FeedbackTrimTicks, settings);
        const f32 slew = SlewForOffsetError(target - input.CurrentOffsetTicks, settings);
        return TickOffsetEstimate{.TargetOffsetTicks = target, .SlewFactor = slew};
    }

    f32 TickOffsetEstimator::Observe(const f32 rttSeconds, const u64 clientTick,
                                     const u64 serverTick)
    {
        if (!m_HasEstimate)
        {
            m_SmoothedRtt = rttSeconds;
            m_SmoothedJitter = 0.0f;
            m_HasEstimate = true;
        }
        else
        {
            const f32 deviation = std::abs(rttSeconds - m_SmoothedRtt);
            m_SmoothedJitter += JitterSmoothing * (deviation - m_SmoothedJitter);
            m_SmoothedRtt += RttSmoothing * (rttSeconds - m_SmoothedRtt);
        }

        const f32 currentOffset =
            static_cast<f32>(static_cast<i64>(clientTick) - static_cast<i64>(serverTick));
        m_Estimate = EstimateTickOffset(TickOffsetInput{.RttSeconds = m_SmoothedRtt,
                                                        .JitterSeconds = m_SmoothedJitter,
                                                        .CurrentOffsetTicks = currentOffset,
                                                        .FeedbackTrimTicks = m_FeedbackTrimTicks},
                                        m_Settings);
        return m_Estimate.SlewFactor;
    }
}
