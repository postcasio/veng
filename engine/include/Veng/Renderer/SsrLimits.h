#pragma once

#include <Veng/Veng.h>

#include <algorithm>

/// @brief The screen-space-reflection trace's authored step budget and its hard clamp.
///
/// Device-free, so the look, the view state and the trace push share one spelling of the budget
/// and its bound.
namespace Veng::Renderer
{
    /// @brief The step budget a view or look carries unless it authors another.
    inline constexpr u32 DefaultSsrMaxSteps = 256;

    /// @brief Upper bound on SsrMaxSteps.
    ///
    /// The budget is a GPU loop bound — each reflected pixel takes at most that many depth tests
    /// and as many hi-Z probes — so an unclamped value is a device hang rather than a recoverable
    /// error. It is clamped where the trace push is built, whatever the view carries.
    inline constexpr u32 SsrMaxStepsCeiling = 4096;

    /// @brief Clamps an authored SSR step budget into the supported range.
    /// @param steps  The authored budget.
    /// @return The value clamped to [1, SsrMaxStepsCeiling].
    [[nodiscard]] inline u32 ClampSsrMaxSteps(const u32 steps)
    {
        return std::clamp(steps, 1u, SsrMaxStepsCeiling);
    }
}
