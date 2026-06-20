#pragma once

#include <Veng/Veng.h>
#include <chrono>

namespace Veng
{
    /// @brief High-resolution clock point.
    using time_point = std::chrono::high_resolution_clock::time_point;

    /// @brief Static time service updated once per frame by the run loop.
    class Time
    {
    public:
        /// @brief Records the start time; called once before the main loop.
        static void Initialize();

        /// @brief Advances the frame clock and returns the delta in seconds.
        /// @return Time in seconds since the previous Update() call.
        static f32 Update();

        /// @brief Returns the time in seconds between the last two frames.
        static f32 GetDeltaTime();

        /// @brief Returns the wall-clock time at the start of the current frame, in seconds since Initialize().
        static f32 GetFrameTime();

        /// @brief Returns the current wall-clock time in seconds since Initialize().
        static f32 Now();

    private:
        /// @brief Seconds between the last two frames.
        static inline f32 s_DeltaTime = 0.0f;
        /// @brief Clock snapshot at the start of the current frame.
        static inline time_point s_FrameTime{};
        /// @brief Clock snapshot at Initialize().
        static inline time_point s_StartTime{};
    };
}
