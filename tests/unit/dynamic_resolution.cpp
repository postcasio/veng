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

// Outer-loop allocation-tier controller cases. StepAllocationTier is pure, clock-free, and
// device-free: it folds a per-frame sub-rect scale into a long EMA and decides which quantized
// allocation tier the targets should be sized at, moving across tiers only through a hysteresis
// band and only after a sustained dwell. These pin the anti-thrash guarantee — no oscillation
// under noise or slow drift, dwell honored, hysteresis respected, a single spike ignored, the
// bounds clamped, and the down-step withheld while the instantaneous scale is too high.

namespace
{
    // The default tiers {1.0, 0.75, 0.5} with the documented dwell/hysteresis defaults.
    AllocationTierSettings TierSettings()
    {
        return AllocationTierSettings{};
    }

    // Drives the controller for a fixed wall-clock duration at a fixed frame delta, feeding a
    // constant sub-rect scale each frame, and returns the resulting tier index.
    u32 RunSteady(AllocationTierState& state, f32 renderScale, f32 seconds, f32 dt,
                  const AllocationTierSettings& settings)
    {
        const int frames = static_cast<int>(seconds / dt);
        u32 tier = state.TierIndex;
        for (int i = 0; i < frames; ++i)
        {
            tier = StepAllocationTier(state, renderScale, dt, settings);
        }
        return tier;
    }
}

TEST_CASE("allocation tier: a scale parked just above a boundary never changes tier (no noise "
          "oscillation)")
{
    const AllocationTierSettings s = TierSettings();
    AllocationTierState state;

    // Park the sub-rect just above the down-threshold of the first boundary (Tiers[1] = 0.75)
    // and jitter it with noise that never sustains a crossing. Over a long run the tier holds.
    f32 jitter = 0.0f;
    for (int i = 0; i < 6000; ++i)
    {
        // Small zero-mean noise around 0.80 — comfortably inside tier 0, above the 0.78 down edge.
        jitter = (i % 2 == 0) ? 0.02f : -0.02f;
        (void)StepAllocationTier(state, 0.80f + jitter, 1.0f / 60.0f, s);
    }
    CHECK(state.TierIndex == 0u);
}

TEST_CASE(
    "allocation tier: a sustained drop steps down only after the shrink dwell, one tier at a time")
{
    const AllocationTierSettings s = TierSettings();
    AllocationTierState state;

    constexpr f32 dt = 1.0f / 60.0f;
    // Feed a scale that comfortably fits tier 1 (0.75): well below 0.75 + DownMargin and below
    // the instantaneous guard 0.75. Before the dwell elapses (with EMA settling), the tier holds.
    RunSteady(state, 0.60f, 1.0f, dt, s); // 1.0 s < ShrinkDwellSeconds 1.5 s, plus EMA lag
    CHECK(state.TierIndex == 0u);

    // After enough sustained time the EMA has crossed the threshold and the dwell elapsed: one
    // step down to tier 1, not two.
    RunSteady(state, 0.60f, 5.0f, dt, s);
    CHECK(state.TierIndex == 1u);

    // Continuing the same sustained load eventually steps to the floor tier 2, one step further.
    RunSteady(state, 0.40f, 10.0f, dt, s);
    CHECK(state.TierIndex == 2u);
}

TEST_CASE("allocation tier: a recovery steps up only after the grow dwell and past the hysteresis")
{
    const AllocationTierSettings s = TierSettings();
    AllocationTierState state;

    constexpr f32 dt = 1.0f / 60.0f;
    // Drive down to tier 1 first.
    RunSteady(state, 0.55f, 8.0f, dt, s);
    REQUIRE(state.TierIndex == 1u);

    // A scale just above tier 1 (0.75) but below the up-threshold (0.75 + UpHysteresis = 0.87)
    // never steps up no matter how long it holds — it is inside the dead band.
    RunSteady(state, 0.82f, 20.0f, dt, s);
    CHECK(state.TierIndex == 1u);

    // A scale above the up-threshold, but for less than the grow dwell, still holds (grow is slow).
    RunSteady(state, 0.95f, 3.0f, dt, s); // < GrowDwellSeconds 5 s, plus EMA lag
    CHECK(state.TierIndex == 1u);

    // Sustained above the up-threshold past the grow dwell: one step up to tier 0.
    RunSteady(state, 0.98f, 10.0f, dt, s);
    CHECK(state.TierIndex == 0u);
}

TEST_CASE("allocation tier: a single-frame spike inside a steady trace never moves the tier")
{
    const AllocationTierSettings s = TierSettings();
    AllocationTierState state;

    constexpr f32 dt = 1.0f / 60.0f;
    // A steady operating point comfortably inside tier 0, interrupted by one-frame spikes far
    // below the down threshold. The long EMA absorbs each isolated spike; the dwell never fills.
    for (int i = 0; i < 6000; ++i)
    {
        const f32 scale = (i % 600 == 0) ? 0.30f : 0.95f;
        (void)StepAllocationTier(state, scale, dt, s);
    }
    CHECK(state.TierIndex == 0u);
}

TEST_CASE("allocation tier: the result is clamped to the tier bounds")
{
    const AllocationTierSettings s = TierSettings();

    // Sustained heavy underuse drives the tier to the floor and pins it there — never past the
    // last index.
    AllocationTierState low;
    for (int i = 0; i < 5000; ++i)
    {
        (void)StepAllocationTier(low, 0.10f, 1.0f / 60.0f, s);
    }
    CHECK(low.TierIndex == static_cast<u32>(s.Tiers.size() - 1));

    // Sustained full use drives the tier to the baseline and pins it there — never below 0.
    AllocationTierState high;
    high.TierIndex = static_cast<u32>(s.Tiers.size() - 1);
    high.SustainedScale = s.Tiers.back();
    for (int i = 0; i < 5000; ++i)
    {
        (void)StepAllocationTier(high, 1.0f, 1.0f / 60.0f, s);
    }
    CHECK(high.TierIndex == 0u);

    // A non-positive delta holds the state and clamps an out-of-range seed back into bounds.
    AllocationTierState held;
    held.TierIndex = 99u;
    const u32 tier = StepAllocationTier(held, 0.5f, 0.0f, s);
    CHECK(tier == static_cast<u32>(s.Tiers.size() - 1));
    CHECK(held.TierIndex == static_cast<u32>(s.Tiers.size() - 1));
}

TEST_CASE("allocation tier: a slow drift across the band produces a bounded transition count")
{
    const AllocationTierSettings s = TierSettings();
    AllocationTierState state;

    constexpr f32 dt = 1.0f / 60.0f;
    constexpr int frames = 120000; // ~33 min of sub-rect scale sweeping the full band
    u32 transitions = 0;
    u32 previous = state.TierIndex;

    // A slow triangle wave spanning the full hysteresis band over many dwell periods. The
    // guarantee is "no limit cycle": a full sweep crosses each boundary a small bounded number
    // of times, not a slow oscillation that racks up transitions.
    for (int i = 0; i < frames; ++i)
    {
        const f32 phase = static_cast<f32>(i % 20000) / 20000.0f;          // 0..1 over ~5.5 min
        const f32 tri = phase < 0.5f ? phase * 2.0f : 2.0f - phase * 2.0f; // 0..1..0
        const f32 scale = 0.45f + tri * 0.55f; // sweeps 0.45 .. 1.0 .. 0.45
        const u32 tier = StepAllocationTier(state, scale, dt, s);
        if (tier != previous)
        {
            ++transitions;
            previous = tier;
        }
    }

    // Six full sweeps over two boundaries: at most a handful of transitions per sweep, never a
    // count that scales with frame count (which a limit cycle would produce).
    CHECK(transitions <= 40u);
}

TEST_CASE(
    "allocation tier: a down-step is withheld while the instantaneous scale exceeds the next tier")
{
    const AllocationTierSettings s = TierSettings();
    AllocationTierState state;

    constexpr f32 dt = 1.0f / 60.0f;
    // Seed the EMA already below the down-threshold for the first boundary (Tiers[1] = 0.75): the
    // sustained signal says "shrink". But feed an instantaneous scale that still rides above the
    // smaller tier (0.80 > 0.75), so the decision-6 guard withholds the step no matter how long
    // it holds.
    state.SustainedScale = 0.70f;
    for (int i = 0; i < 6000; ++i)
    {
        (void)StepAllocationTier(state, 0.80f, dt, s);
    }
    CHECK(state.TierIndex == 0u);

    // The moment the instantaneous scale also fits the smaller tier, the held-back step fires
    // after the dwell.
    for (int i = 0; i < 200; ++i)
    {
        (void)StepAllocationTier(state, 0.70f, dt, s);
    }
    CHECK(state.TierIndex == 1u);
}
