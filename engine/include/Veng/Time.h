#pragma once

#include <Veng/Veng.h>
#include <chrono>

namespace Veng
{
    using time_point = std::chrono::high_resolution_clock::time_point;

    class Time
    {
    public:
        static void Initialize();
        static f32 Update();

        static f32 GetDeltaTime();
        static f32 GetFrameTime();
        static f32 Now();

    private:
        static inline f32 s_DeltaTime = 0.0f;
        static inline time_point s_FrameTime{};
        static inline time_point s_StartTime{};
    };
}
