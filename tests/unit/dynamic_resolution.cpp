// Adaptive-resolution controller cases. ComputeDynamicResolutionScale is pure and device-free:
// it maps a measured GPU frame time to the next render scale. These pin the controller's
// behavior — over budget reduces, headroom raises, the deadband holds, the rate limit caps a
// single step, the bounds clamp, and a non-positive measurement holds — with no device.

#include <doctest/doctest.h>

#include <Veng/Renderer/DynamicResolution.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // 60 Hz budget, half-resolution floor, full-resolution ceiling, gentle rate limit.
    DynamicResolutionSettings Settings()
    {
        return {
            .TargetFrameTimeMs = 1000.0f / 60.0f,
            .MinScale = 0.5f,
            .MaxScale = 1.0f,
            .Headroom = 0.1f,
            .MaxStep = 0.1f,
        };
    }
}

TEST_CASE("dynamic resolution: over budget reduces the scale, capped by the rate limit")
{
    const DynamicResolutionSettings s = Settings();

    // 25 ms against a 16.67 ms budget: well over, so the scale drops — but no more than MaxStep
    // in a single update, so a spike can't collapse the resolution at once.
    const f32 next = ComputeDynamicResolutionScale(1.0f, 25.0f, s);
    CHECK(next < 1.0f);
    CHECK(next == doctest::Approx(0.9f));
}

TEST_CASE("dynamic resolution: comfortably under budget raises the scale, capped by the rate limit")
{
    const DynamicResolutionSettings s = Settings();

    // 8 ms against a 16.67 ms budget, starting low: there is headroom, so the scale rises by at
    // most MaxStep.
    const f32 next = ComputeDynamicResolutionScale(0.5f, 8.0f, s);
    CHECK(next > 0.5f);
    CHECK(next == doctest::Approx(0.6f));
}

TEST_CASE("dynamic resolution: a measurement in the deadband holds the scale")
{
    const DynamicResolutionSettings s = Settings();

    // 16 ms sits inside [target*(1-headroom), target] = [15, 16.67]: neither over budget nor
    // comfortably under, so the scale is unchanged.
    CHECK(ComputeDynamicResolutionScale(0.8f, 16.0f, s) == doctest::Approx(0.8f));
}

TEST_CASE("dynamic resolution: a non-positive measurement holds the scale (clamped into range)")
{
    const DynamicResolutionSettings s = Settings();

    // No timing this frame: hold.
    CHECK(ComputeDynamicResolutionScale(0.75f, 0.0f, s) == doctest::Approx(0.75f));
    CHECK(ComputeDynamicResolutionScale(0.75f, -1.0f, s) == doctest::Approx(0.75f));
    // A current scale outside the bounds is clamped back even with no measurement.
    CHECK(ComputeDynamicResolutionScale(2.0f, 0.0f, s) == doctest::Approx(1.0f));
    CHECK(ComputeDynamicResolutionScale(0.1f, 0.0f, s) == doctest::Approx(0.5f));
}

TEST_CASE("dynamic resolution: the scale never leaves [MinScale, MaxScale]")
{
    const DynamicResolutionSettings s = Settings();

    // Sustained heavy overshoot drives the scale to the floor and pins it there.
    f32 scale = 1.0f;
    for (int i = 0; i < 50; ++i)
    {
        scale = ComputeDynamicResolutionScale(scale, 100.0f, s);
    }
    CHECK(scale == doctest::Approx(s.MinScale));

    // Sustained idle drives the scale to the ceiling and pins it there.
    scale = s.MinScale;
    for (int i = 0; i < 50; ++i)
    {
        scale = ComputeDynamicResolutionScale(scale, 1.0f, s);
    }
    CHECK(scale == doctest::Approx(s.MaxScale));
}

TEST_CASE("dynamic resolution: the scale converges into the deadband and then holds")
{
    const DynamicResolutionSettings s = Settings();

    // A toy cost model: GPU time is proportional to rendered pixels (scale^2). Start over budget
    // and feed the controller its own predicted cost each step; it must settle without
    // oscillating, landing at or just under the budget.
    constexpr f32 costAtFullScale = 30.0f; // ms at scale 1.0 — over the 16.67 ms budget
    f32 scale = 1.0f;
    for (int i = 0; i < 200; ++i)
    {
        const f32 gpuMs = costAtFullScale * scale * scale;
        scale = ComputeDynamicResolutionScale(scale, gpuMs, s);
    }

    const f32 settledMs = costAtFullScale * scale * scale;
    // Settled inside (or just under) the deadband: not over budget, not far under it.
    CHECK(settledMs <= s.TargetFrameTimeMs + 0.01f);
    CHECK(settledMs >= s.TargetFrameTimeMs * (1.0f - s.Headroom) - 0.5f);

    // And it is stable: another step from the settled point does not move it.
    const f32 held = ComputeDynamicResolutionScale(scale, settledMs, s);
    CHECK(held == doctest::Approx(scale));
}
