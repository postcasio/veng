#pragma once

#include <Veng/Veng.h>

namespace Veng
{
    /// @brief A best-effort frame-rate cap: a sleep-to-deadline limiter over a monotonic clock.
    ///
    /// Independent of present-mode vsync — a player may want a 60 fps cap with vsync off — so the run
    /// loop consults it once per frame regardless of the present mode. It owns only the target period
    /// and the next-frame deadline; the caller supplies the current time and performs the sleep the
    /// limiter returns, so the class is device- and clock-free and therefore unit-testable against a
    /// synthetic clock.
    ///
    /// It is best-effort: a caller's sleep is not exact, and a frame that overruns its budget is not
    /// clawed back (the deadline resets to now rather than accumulating debt), so the limiter bounds
    /// the rate from above without ever spinning to hit it.
    class FrameRateLimiter
    {
    public:
        /// @brief Sets the cap in frames per second; 0 disables the limiter (uncapped).
        ///
        /// Changing the cap re-arms the deadline on the next AcquireSleepSeconds, so a mid-run
        /// change takes effect without a stored deadline from the old rate pacing one frame wrong.
        /// @param hz  The target frames per second, or 0 for uncapped.
        void SetCapHz(u32 hz)
        {
            m_CapHz = hz;
            m_Armed = false;
        }

        /// @brief Returns the current cap in frames per second; 0 when uncapped.
        [[nodiscard]] u32 GetCapHz() const { return m_CapHz; }

        /// @brief Advances the deadline by one frame and returns how long to sleep to honor the cap.
        ///
        /// Call once per frame with the current monotonic time in seconds; sleep for the returned
        /// duration (0 means do not sleep). Uncapped (cap 0) always returns 0. When the frame ran
        /// long and the deadline is already past, the deadline resets to @p nowSeconds and 0 is
        /// returned, so a slow stretch is not paid back by starving later frames.
        /// @param nowSeconds  The current monotonic time, in seconds.
        /// @return The number of seconds to sleep before starting the next frame; 0 when uncapped or
        ///         already behind the deadline.
        [[nodiscard]] f64 AcquireSleepSeconds(f64 nowSeconds)
        {
            if (m_CapHz == 0)
            {
                m_Armed = false;
                return 0.0;
            }

            const f64 period = 1.0 / static_cast<f64>(m_CapHz);
            if (!m_Armed)
            {
                m_NextDeadline = nowSeconds + period;
                m_Armed = true;
                return period;
            }

            m_NextDeadline += period;
            const f64 sleep = m_NextDeadline - nowSeconds;
            if (sleep <= 0.0)
            {
                // Fell behind: do not accumulate debt that would starve later frames.
                m_NextDeadline = nowSeconds;
                return 0.0;
            }
            return sleep;
        }

    private:
        /// @brief The target frames per second; 0 is uncapped.
        u32 m_CapHz = 0;
        /// @brief Whether m_NextDeadline holds a live deadline (cleared on a cap change).
        bool m_Armed = false;
        /// @brief The monotonic time the next frame should start at, in seconds.
        f64 m_NextDeadline = 0.0;
    };
}
