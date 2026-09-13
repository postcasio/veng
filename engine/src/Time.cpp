#include <Veng/Time.h>

namespace Veng
{
    void Time::Initialize()
    {
        s_StartTime = std::chrono::high_resolution_clock::now();
        s_FrameTime = s_StartTime;
        s_DeltaTime = 0.0f;
        // A fresh clock's last wall sample is zero, which is exactly "seconds since s_StartTime" at
        // this instant, so the first Update measures from Initialize as it always has.
        s_Clock = FrameClock{};
        s_DriveFrameTime = 0.0f;
    }

    f32 Time::Update()
    {
        const auto now = std::chrono::high_resolution_clock::now();

        s_DeltaTime = s_Clock.Update(std::chrono::duration<f64>(now - s_StartTime).count());
        s_FrameTime = now;

        return s_DeltaTime;
    }

    f32 Time::GetDeltaTime()
    {
        return s_DeltaTime;
    }

    f32 Time::GetFrameTime()
    {
        if (s_Clock.IsDriven())
        {
            return s_DriveFrameTime + static_cast<f32>(s_Clock.GetDrivenSeconds());
        }
        return std::chrono::duration<f32>(s_FrameTime - s_StartTime).count();
    }

    f32 Time::Now()
    {
        return std::chrono::duration<f32>(std::chrono::high_resolution_clock::now() - s_StartTime)
            .count();
    }

    void Time::Drive(FrameClockInfo info)
    {
        // The base is the frame time as it stands now, so the driven value continues from the last
        // wall frame rather than restarting at zero.
        s_DriveFrameTime = GetFrameTime();
        s_Clock.Drive(info);
    }

    void Time::Release()
    {
        s_Clock.Release();
    }

    bool Time::IsDriven()
    {
        return s_Clock.IsDriven();
    }

    f64 Time::GetDrivenSeconds()
    {
        return s_Clock.GetDrivenSeconds();
    }

    u64 Time::GetDrivenFrames()
    {
        return s_Clock.GetDrivenFrames();
    }
}
