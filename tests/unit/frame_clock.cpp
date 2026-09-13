// The frame clock's two modes, proven against a synthetic wall clock because Update takes the wall
// time as a parameter: wall mode reports successive wall differences, driven mode reports its fixed
// delta whatever the wall says and accumulates it, and a release measures from the most recent
// Update rather than from where the drive began. Time is exercised through the same class for the
// one thing only it owns — the frame time the shader clock reads following the mode.

#include <doctest/doctest.h>

#include <Veng/FrameClock.h>
#include <Veng/Time.h>

#include <algorithm>
#include <cmath>
#include <limits>

using namespace Veng;

TEST_CASE("frame clock: wall mode returns successive wall differences")
{
    FrameClock clock;
    CHECK_FALSE(clock.IsDriven());

    CHECK(clock.Update(0.25) == doctest::Approx(0.25));
    CHECK(clock.Update(0.5) == doctest::Approx(0.25));
    CHECK(clock.Update(0.51) == doctest::Approx(0.01));
    CHECK(clock.Update(0.51) == doctest::Approx(0.0));

    // The driven accumulators read zero for a clock that has never been driven.
    CHECK(clock.GetDrivenSeconds() == doctest::Approx(0.0));
    CHECK(clock.GetDrivenFrames() == 0);
}

TEST_CASE(
    "frame clock: a driven clock returns its delta whatever the wall says, and accumulates it")
{
    constexpr f32 delta = 1.0f / 60.0f;

    FrameClock clock;
    CHECK(clock.Update(1.0) == doctest::Approx(1.0));

    clock.Drive({.Delta = delta});
    CHECK(clock.IsDriven());

    // The wall argument is ignored in both directions: a frame that took two seconds and one that
    // took none both report the driven delta.
    CHECK(clock.Update(3.0) == doctest::Approx(delta));
    CHECK(clock.Update(3.0) == doctest::Approx(delta));
    CHECK(clock.Update(100.0) == doctest::Approx(delta));

    // A thousand more frames: the accumulated time is the frame count times the delta, asserted once
    // rather than per iteration.
    f32 smallest = std::numeric_limits<f32>::max();
    f32 largest = 0.0f;
    for (u32 i = 0; i < 1000; ++i)
    {
        const f32 step = clock.Update(100.0);
        smallest = std::min(smallest, step);
        largest = std::max(largest, step);
    }
    CHECK(smallest == doctest::Approx(delta));
    CHECK(largest == doctest::Approx(delta));

    CHECK(clock.GetDrivenFrames() == 1003);
    const f64 expected = static_cast<f64>(clock.GetDrivenFrames()) * static_cast<f64>(delta);
    CHECK(std::abs(clock.GetDrivenSeconds() - expected) < 1e-6);
}

TEST_CASE("frame clock: a release measures from the most recent Update, not from the drive")
{
    FrameClock clock;
    CHECK(clock.Update(0.0) == doctest::Approx(0.0));

    clock.Drive({.Delta = 0.01f});
    for (u32 i = 0; i < 3; ++i)
    {
        CHECK(clock.Update(100.0) == doctest::Approx(0.01));
    }

    clock.Release();
    CHECK_FALSE(clock.IsDriven());
    CHECK(clock.GetDrivenSeconds() == doctest::Approx(0.0));
    CHECK(clock.GetDrivenFrames() == 0);

    // 0.016 since the last Update's wall time — not the 100 seconds since the drive began.
    CHECK(clock.Update(100.016) == doctest::Approx(0.016));
}

TEST_CASE("frame clock: re-driving re-bases the accumulators at the new delta")
{
    FrameClock clock;
    clock.Drive({.Delta = 0.01f});
    CHECK(clock.Update(0.0) == doctest::Approx(0.01));
    CHECK(clock.Update(0.0) == doctest::Approx(0.01));

    clock.Drive({.Delta = 0.02f});
    CHECK(clock.GetDrivenFrames() == 0);
    CHECK(clock.GetDrivenSeconds() == doctest::Approx(0.0));
    CHECK(clock.Update(0.0) == doctest::Approx(0.02));
    CHECK(clock.GetDrivenFrames() == 1);
}

TEST_CASE("time: the frame time the shader clock reads follows the driven mode")
{
    constexpr f32 delta = 0.25f;

    Time::Initialize();
    Time::Update();
    CHECK_FALSE(Time::IsDriven());

    // The base is the wall frame time as it stands at the drive, so the driven value continues from
    // the last wall frame rather than restarting at zero.
    const f32 base = Time::GetFrameTime();

    Time::Drive({.Delta = delta});
    CHECK(Time::IsDriven());

    for (u32 i = 1; i <= 4; ++i)
    {
        CHECK(Time::Update() == doctest::Approx(delta));
        CHECK(Time::GetDeltaTime() == doctest::Approx(delta));
        CHECK(Time::GetFrameTime() == doctest::Approx(base + static_cast<f32>(i) * delta));
    }
    CHECK(Time::GetDrivenFrames() == 4);
    CHECK(Time::GetDrivenSeconds() == doctest::Approx(4.0 * static_cast<f64>(delta)));

    // Now() is wall time in both modes: it has not advanced a full driven second.
    CHECK(Time::Now() < base + delta);

    Time::Release();
    CHECK_FALSE(Time::IsDriven());
    CHECK(Time::GetDrivenFrames() == 0);
    CHECK(Time::GetFrameTime() < base + delta);
}
