#pragma once

#include <Veng/FrameClock.h>
#include <Veng/Veng.h>
#include <chrono>

namespace Veng
{
    /// @brief High-resolution clock point.
    using time_point = std::chrono::high_resolution_clock::time_point;

    /// @brief Static time service updated once per frame by the run loop.
    ///
    /// It carries a FrameClock, so the frame time the run loop hands every consumer is either the
    /// wall clock or a caller-driven fixed delta. **GetDeltaTime() and GetFrameTime() follow the
    /// mode**: the first is the delta the run loop distributes, the second the engine-global shader
    /// clock animated materials read, and a driven frame must advance both by its nominal step
    /// rather than by however long it actually took. **Now() does not follow the mode** — it stays
    /// wall time in both, because session timeouts, directory reaping and the net pumps must keep
    /// running at real cadence while the simulation is driven.
    class Time
    {
    public:
        /// @brief Records the start time; called once before the main loop.
        static void Initialize();

        /// @brief Advances the frame clock and returns the delta in seconds.
        /// @return The driven delta while driven, else the time in seconds since the previous
        ///         Update() call.
        static f32 Update();

        /// @brief Returns the time in seconds between the last two frames.
        static f32 GetDeltaTime();

        /// @brief Returns the frame clock at the start of the current frame, in seconds since
        ///        Initialize().
        ///
        /// Wall time in wall mode. While driven it is the wall time at Drive() plus the accumulated
        /// driven time, so the value is continuous across the transition and advances one driven
        /// delta per frame.
        static f32 GetFrameTime();

        /// @brief Returns the current wall-clock time in seconds since Initialize().
        ///
        /// Wall time in both modes — the clock for work that must run at real cadence regardless of
        /// how the frame clock is driven.
        static f32 Now();

        /// @brief Enters driven mode: every Update returns a fixed delta instead of measuring.
        /// @param info  The driven-mode descriptor.
        /// @pre info.Delta > 0 — asserted otherwise.
        static void Drive(FrameClockInfo info);

        /// @brief Returns the frame clock to wall mode.
        static void Release();

        /// @brief Returns whether the frame clock is driven.
        [[nodiscard]] static bool IsDriven();

        /// @brief Returns the driven time accumulated since Drive(), in seconds; 0 in wall mode.
        [[nodiscard]] static f64 GetDrivenSeconds();

        /// @brief Returns the number of driven frames since Drive(); 0 in wall mode.
        [[nodiscard]] static u64 GetDrivenFrames();

    private:
        /// @brief Seconds between the last two frames, as the mode reports them.
        static inline f32 s_DeltaTime = 0.0f;
        /// @brief Clock snapshot at the start of the current frame.
        static inline time_point s_FrameTime{};
        /// @brief Clock snapshot at Initialize().
        static inline time_point s_StartTime{};
        /// @brief The mode-carrying frame clock behind Update().
        static inline FrameClock s_Clock;
        /// @brief GetFrameTime()'s wall value at the most recent Drive(), the driven time's base.
        static inline f32 s_DriveFrameTime = 0.0f;
    };
}
