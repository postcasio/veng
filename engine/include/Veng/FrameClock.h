#pragma once

#include <Veng/Assert.h>
#include <Veng/Veng.h>

namespace Veng
{
    /// @brief Descriptor for entering the frame clock's driven mode.
    struct FrameClockInfo
    {
        /// @brief The delta in seconds every driven Update returns; must be > 0.
        ///
        /// Any positive value is legal: 1/fps reproduces a fixed frame rate, and a value that is not
        /// the reciprocal of a frame rate simply advances the world by that much per frame.
        f32 Delta = 1.0f / 60.0f;
    };

    /// @brief The run loop's frame clock: wall time, or a fixed delta the caller supplies.
    ///
    /// In **wall** mode Update returns the elapsed wall time between successive calls. In **driven**
    /// mode it returns FrameClockInfo::Delta regardless of how long the frame actually took, and
    /// accumulates that delta into a driven frame time — so a frame that took two seconds to produce
    /// still advances the world by its nominal step.
    ///
    /// Update takes the wall clock as a parameter rather than sampling one, so the class is
    /// clock-free and testable against a synthetic clock (the idiom FrameRateLimiter uses). The wall
    /// sample is recorded in both modes, so the first wall frame after Release measures from the most
    /// recent Update rather than from the moment the drive began.
    class FrameClock
    {
    public:
        /// @brief Enters driven mode; the next Update returns @p info.Delta.
        ///
        /// Resets the driven accumulators, so GetDrivenSeconds() and GetDrivenFrames() count from
        /// this call. Driving an already-driven clock re-bases it at the new delta.
        /// @param info  The driven-mode descriptor.
        /// @pre info.Delta > 0 — asserted otherwise.
        void Drive(FrameClockInfo info)
        {
            VE_ASSERT(info.Delta > 0.0f, "FrameClock Delta must be > 0 (got {})", info.Delta);

            m_Driven = true;
            m_Delta = info.Delta;
            m_DrivenSeconds = 0.0;
            m_DrivenFrames = 0;
        }

        /// @brief Returns to wall mode and clears the driven accumulators.
        ///
        /// The next Update measures from the wall time of the most recent Update, not from when the
        /// drive began, so the first released frame reports an ordinary frame's delta.
        void Release()
        {
            m_Driven = false;
            m_DrivenSeconds = 0.0;
            m_DrivenFrames = 0;
        }

        /// @brief Returns whether the clock is in driven mode.
        [[nodiscard]] bool IsDriven() const { return m_Driven; }

        /// @brief Advances the clock by one frame and returns that frame's delta in seconds.
        ///
        /// @param wallNowSeconds  The current wall-clock time in seconds, on any fixed origin.
        /// @return The driven delta while driven, else the wall time since the previous Update.
        [[nodiscard]] f32 Update(f64 wallNowSeconds)
        {
            const f32 delta =
                m_Driven ? m_Delta : static_cast<f32>(wallNowSeconds - m_LastWallSeconds);
            m_LastWallSeconds = wallNowSeconds;

            if (m_Driven)
            {
                m_DrivenSeconds += static_cast<f64>(m_Delta);
                ++m_DrivenFrames;
            }

            return delta;
        }

        /// @brief Returns the driven time accumulated since Drive(), in seconds; 0 in wall mode.
        [[nodiscard]] f64 GetDrivenSeconds() const { return m_DrivenSeconds; }

        /// @brief Returns the number of driven frames returned since Drive(); 0 in wall mode.
        [[nodiscard]] u64 GetDrivenFrames() const { return m_DrivenFrames; }

    private:
        /// @brief Whether the clock returns the fixed delta instead of measuring the wall clock.
        bool m_Driven = false;
        /// @brief The delta returned per driven frame, in seconds.
        f32 m_Delta = 1.0f / 60.0f;
        /// @brief The wall time handed to the most recent Update, in seconds.
        f64 m_LastWallSeconds = 0.0;
        /// @brief Driven time accumulated since Drive(), in seconds.
        f64 m_DrivenSeconds = 0.0;
        /// @brief Driven frames returned since Drive().
        u64 m_DrivenFrames = 0;
    };
}
