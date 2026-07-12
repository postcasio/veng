// Fixed-timestep accumulator cases. SimClock is pure and device-free: it folds a frame delta into
// an accumulator, runs the whole fixed steps that have accumulated (clamped against the spiral of
// death), advances a monotonic tick, and reports the residual interpolation alpha. These pin its
// behavior over scripted frame-delta sequences — step counts, tick numbers, alpha, the zero-tick
// frame, the catch-up, the clamp, and the pause reset — with no device.

#include <doctest/doctest.h>

#include <Veng/Scene/SimClock.h>

using namespace Veng;

namespace
{
    // 60 Hz sim, the default 5-tick spiral-of-death clamp.
    SimClock Make(const u32 rate = 60, const u32 maxTicks = 5)
    {
        return SimClock(SimClockInfo{.TickRate = rate, .MaxTicksPerFrame = maxTicks});
    }

    constexpr f32 Step60 = 1.0f / 60.0f;
}

TEST_CASE("sim clock: a frame at exactly the tick rate runs one tick, tick 1, zero alpha")
{
    SimClock clock = Make();
    const SimStep step = clock.Advance(Step60);

    CHECK(step.Steps == 1);
    CHECK(step.FirstTick == 1);
    CHECK(step.SimDelta == doctest::Approx(Step60));
    CHECK(step.Alpha == doctest::Approx(0.0f));
    CHECK(clock.GetTick() == 1);
}

TEST_CASE(
    "sim clock: a frame above the tick rate runs zero ticks and carries the residual as alpha")
{
    SimClock clock = Make();

    // 120 fps against a 60 Hz sim: each frame accumulates half a step, so the first frame runs no
    // tick and reports alpha ~0.5; the second frame completes the step.
    const SimStep first = clock.Advance(Step60 * 0.5f);
    CHECK(first.Steps == 0);
    CHECK(first.FirstTick == 1);
    CHECK(first.Alpha == doctest::Approx(0.5f));
    CHECK(clock.GetTick() == 0);

    const SimStep second = clock.Advance(Step60 * 0.5f);
    CHECK(second.Steps == 1);
    CHECK(second.FirstTick == 1);
    CHECK(second.Alpha == doctest::Approx(0.0f));
    CHECK(clock.GetTick() == 1);
}

TEST_CASE("sim clock: a slow frame runs several catch-up ticks with a contiguous tick range")
{
    SimClock clock = Make();

    // First advance one tick so the range does not start at 1.
    clock.Advance(Step60);
    REQUIRE(clock.GetTick() == 1);

    // A 3.5-step frame runs three whole ticks (2, 3, 4) and carries half a step as alpha.
    const SimStep step = clock.Advance(Step60 * 3.5f);
    CHECK(step.Steps == 3);
    CHECK(step.FirstTick == 2);
    CHECK(step.Alpha == doctest::Approx(0.5f));
    CHECK(clock.GetTick() == 4);
}

TEST_CASE("sim clock: the spiral-of-death clamp bounds the step count and drops the backlog")
{
    SimClock clock = Make(60, 5);

    // A one-second stall would owe 60 ticks; the clamp runs at most 5 and drops the rest, so alpha
    // is zero (no residual chased) and the tick advanced by exactly the clamp.
    const SimStep step = clock.Advance(1.0f);
    CHECK(step.Steps == 5);
    CHECK(step.FirstTick == 1);
    CHECK(step.Alpha == doctest::Approx(0.0f));
    CHECK(step.Clamped); // the frame dropped backlog — the clock now trails wall-clock time
    CHECK(clock.GetTick() == 5);

    // The dropped backlog does not resurface: the next ordinary frame runs a single tick, unclamped.
    const SimStep next = clock.Advance(Step60);
    CHECK(next.Steps == 1);
    CHECK(next.FirstTick == 6);
    CHECK_FALSE(next.Clamped);
    CHECK(clock.GetTick() == 6);
}

TEST_CASE("sim clock: SetTick jumps the tick epoch and clears the accumulator")
{
    SimClock clock = Make();
    clock.Advance(Step60 * 1.5f); // tick 1, half a step of residual
    REQUIRE(clock.GetTick() == 1);

    // A networked client re-snapping its epoch to the server's tick: jump forward, residual cleared.
    clock.SetTick(1000);
    CHECK(clock.GetTick() == 1000);

    // The next ordinary frame continues from the seeded tick with no carried residual.
    const SimStep step = clock.Advance(Step60);
    CHECK(step.FirstTick == 1001);
    CHECK(clock.GetTick() == 1001);
    CHECK(step.Alpha == doctest::Approx(0.0f));
}

TEST_CASE("sim clock: reset drops the accumulator without moving the tick (no pause debt)")
{
    SimClock clock = Make();

    clock.Advance(Step60);
    REQUIRE(clock.GetTick() == 1);

    // Accumulate three-quarters of a step, then reset (a pause): the residual is dropped and the
    // tick stays put, so resuming chases no backlog.
    clock.Advance(Step60 * 0.75f);
    clock.Reset();
    CHECK(clock.GetTick() == 1);

    const SimStep resumed = clock.Advance(Step60 * 0.5f);
    CHECK(resumed.Steps == 0);
    CHECK(resumed.Alpha == doctest::Approx(0.5f));
    CHECK(clock.GetTick() == 1);
}

TEST_CASE("sim clock: sub-step residuals carry across frames without losing or inventing a tick")
{
    SimClock clock = Make();

    // Four half-step frames: the residual carries across frames, so a whole step completes only on
    // the frames that top the accumulator up to a full step — two ticks over four frames, never
    // dropped and never doubled. Halving is exact in float, so the accumulator lands on zero.
    CHECK(clock.Advance(Step60 * 0.5f).Steps == 0); // 0.5 accumulated
    CHECK(clock.Advance(Step60 * 0.5f).Steps == 1); // 1.0 accumulated: one tick
    CHECK(clock.Advance(Step60 * 0.5f).Steps == 0); // 0.5 accumulated
    CHECK(clock.Advance(Step60 * 0.5f).Steps == 1); // 1.0 accumulated: one tick

    CHECK(clock.GetTick() == 2);
}
