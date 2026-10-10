#include "GeneratedTextureBudget.h"

#include <algorithm>
#include <cmath>

namespace Veng::Renderer
{
    GeneratedTextureBudgetController::GeneratedTextureBudgetController(const u32 floor)
        : m_Floor(std::max(1u, floor)), m_Budget(m_Floor)
    {
    }

    void GeneratedTextureBudgetController::Observe(const f32 pumpMs, const u64 costSpent)
    {
        if (costSpent == 0 || !(pumpMs > 0.0f))
        {
            return;
        }
        const f32 sample = pumpMs / static_cast<f32>(costSpent);
        m_MsPerCost =
            m_MsPerCost > 0.0f ? m_MsPerCost + (CostSmoothing * (sample - m_MsPerCost)) : sample;
    }

    f32 GeneratedTextureBudgetController::ResolvePeriodMs(const GeneratedTextureFrameHint& hint)
    {
        if (hint.MeasuredDeltaSeconds > 0.0f)
        {
            const f32 deltaMs = std::min(hint.MeasuredDeltaSeconds * 1000.0f, MaxPeriodMs);
            m_SmoothedDeltaMs =
                m_SmoothedDeltaMs > 0.0f
                    ? m_SmoothedDeltaMs + (PeriodSmoothing * (deltaMs - m_SmoothedDeltaMs))
                    : deltaMs;
        }
        // A cap names the period the run loop paces to, which a measured delta only approaches.
        if (hint.FrameCapHz > 0)
        {
            return std::min(1000.0f / static_cast<f32>(hint.FrameCapHz), MaxPeriodMs);
        }
        return m_SmoothedDeltaMs;
    }

    u32 GeneratedTextureBudgetController::ResolveFallback(
        const f32 periodMs, const GeneratedTextureBudgetSettings& settings)
    {
        const f32 referenceHz =
            settings.FallbackReferenceHz > 0.0f ? settings.FallbackReferenceHz : 60.0f;
        const f32 scale = std::clamp(periodMs * referenceHz / 1000.0f, 1.0f, MaxFallbackScale);
        // The fraction a frame cannot spend carries into the next, so the average follows the
        // period exactly rather than rounding every frame down.
        m_FallbackCarry += static_cast<f32>(m_Floor) * scale;
        const f32 whole = std::floor(m_FallbackCarry);
        m_FallbackCarry -= whole;
        return std::max(m_Floor, static_cast<u32>(whole));
    }

    u32 GeneratedTextureBudgetController::Resolve(const GeneratedTextureBudgetInput& input,
                                                  const GeneratedTextureBudgetSettings& settings)
    {
        m_TargetMs = 0.0f;
        m_PeriodMs = ResolvePeriodMs(input.Hint);

        // Every path but the timed and fallback ones spends the floor and forgets any boost, so a
        // driven run, an idle service and a context with no pacing are reproducible frame for frame.
        if (!settings.Enabled || input.Hint.Driven || m_PeriodMs <= 0.0f || !input.HasWork)
        {
            m_FallbackCarry = 0.0f;
            m_Budget = m_Floor;
            return m_Budget;
        }

        if (!input.TimingSupported)
        {
            m_Budget = ResolveFallback(m_PeriodMs, settings);
            return m_Budget;
        }

        u32 desired = m_Floor;
        if (m_MsPerCost > 0.0f && input.GpuFrameMs > 0.0f)
        {
            const f32 margin = std::clamp(settings.FrameMargin, 0.0f, 1.0f);
            const f32 share = std::clamp(settings.IdleShare, 0.0f, 1.0f);
            const f32 otherMs = std::max(0.0f, input.GpuFrameMs - input.PumpMs);
            const f32 headroomMs = (m_PeriodMs * (1.0f - margin)) - otherMs;
            m_TargetMs =
                std::clamp(headroomMs * share, 0.0f, std::max(0.0f, settings.MaxMilliseconds));
            const f32 cost = std::min(m_TargetMs / m_MsPerCost, static_cast<f32>(MaxBudget));
            desired = std::max(m_Floor, static_cast<u32>(cost));
        }

        // Fall at once — a frame that lost its headroom must not keep paying for it — and rise by a
        // fraction of the gap, since the measurement that justifies a rise is two frames old.
        if (desired <= m_Budget)
        {
            m_Budget = desired;
        }
        else
        {
            const auto step =
                static_cast<u32>(std::ceil(static_cast<f32>(desired - m_Budget) * RiseRate));
            m_Budget = std::min(desired, m_Budget + std::max(1u, step));
        }
        return m_Budget;
    }

    f32 FindTopLevelGpuScopeMs(const std::span<const Context::GpuPassTiming> timings,
                               const string_view name)
    {
        for (const Context::GpuPassTiming& timing : timings)
        {
            if (timing.Depth == 0 && timing.Name == name)
            {
                return timing.Milliseconds;
            }
        }
        return 0.0f;
    }
}
