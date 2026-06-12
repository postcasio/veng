#include <Veng/Time.h>


namespace Veng
{
    void Time::Initialize()
    {
        s_StartTime = std::chrono::high_resolution_clock::now();
    }

    f32 Time::Update()
    {
        auto now = std::chrono::high_resolution_clock::now();

        s_DeltaTime = std::chrono::duration<f32>(now - s_FrameTime).count();
        s_FrameTime = now;

        return s_DeltaTime;
    }

    f32 Time::GetDeltaTime()
    {
        return s_DeltaTime;
    }

    f32 Time::GetFrameTime()
    {
        return std::chrono::duration<f32>(s_FrameTime - s_StartTime).count();
    }

    f32 Time::Now()
    {
        return std::chrono::duration<f32>(std::chrono::high_resolution_clock::now() - s_StartTime).count();
    }
}
