// Fixed-timestep accumulator cases. SimClock is pure and device-free: it folds a frame delta into
// an accumulator, runs the whole fixed steps that have accumulated (clamped against the spiral of
// death), advances a monotonic tick, and reports the residual interpolation alpha. These pin its
// behavior over scripted frame-delta sequences — step counts, tick numbers, alpha, the zero-tick
// frame, the catch-up, the clamp, the pause reset, and the wall-clock budget (against a scripted
// clock) — with no device.

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

TEST_CASE("sim clock: a frame that fills the clamp exactly keeps its residual as alpha")
{
    SimClock clock = Make(60, 5);

    // 5.5 steps runs the five the clamp allows and owes less than one more, so nothing is dropped.
    const SimStep step = clock.Advance(Step60 * 5.5f);
    CHECK(step.Steps == 5);
    CHECK_FALSE(step.Clamped);
    CHECK(step.Alpha == doctest::Approx(0.5f));

    // The carried half step completes on the next half-step frame.
    const SimStep next = clock.Advance(Step60 * 0.5f);
    CHECK(next.Steps == 1);
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

namespace
{
    // A scripted wall clock: each step a Run hands the callback advances it by a fixed cost, so a
    // budget's stopping point is exact and the steps' wall times are known.
    struct ScriptedClock
    {
        f64 Now = 0.0;
        f64 StepCost = 0.0;
        vector<SimStepInfo> Steps;
        vector<f64> StartedAt;

        auto Step()
        {
            return [this](const SimStepInfo& info)
            {
                Steps.push_back(info);
                StartedAt.push_back(Now);
                Now += StepCost;
                return true;
            };
        }

        auto Clock()
        {
            return [this] { return Now; };
        }
    };
}

TEST_CASE("sim clock: a run marks the frame's first and last step and records its final two")
{
    SimClock clock = Make();
    ScriptedClock script;

    const SimStep step = clock.Run(Step60 * 5.5f, script.Step(), script.Clock());
    REQUIRE(step.Steps == 5);
    REQUIRE(script.Steps.size() == 5);

    u32 firsts = 0;
    u32 lasts = 0;
    u32 records = 0;
    for (usize i = 0; i < script.Steps.size(); ++i)
    {
        const SimStepInfo& info = script.Steps[i];
        CHECK(info.Tick == step.FirstTick + i);
        CHECK(info.Delta == doctest::Approx(Step60));
        firsts += info.First ? 1 : 0;
        lasts += info.Last ? 1 : 0;
        records += info.RecordsHistory ? 1 : 0;
    }
    CHECK(firsts == 1);
    CHECK(lasts == 1);
    CHECK(records == 2);
    CHECK(script.Steps.front().First);
    CHECK(script.Steps.back().Last);
    CHECK(script.Steps[3].RecordsHistory);
    CHECK(script.Steps[4].RecordsHistory);

    // A one-step frame is its own first and last, and records.
    script.Steps.clear();
    clock.Run(Step60, script.Step(), script.Clock());
    REQUIRE(script.Steps.size() == 1);
    CHECK(script.Steps[0].First);
    CHECK(script.Steps[0].Last);
    CHECK(script.Steps[0].RecordsHistory);
}

TEST_CASE("sim clock: a budget ends the frame at the step that spends it and drops the backlog")
{
    SimClock clock(
        SimClockInfo{.TickRate = 60, .MaxTicksPerFrame = 5, .MaxSimMillisecondsPerFrame = 10.0f});
    ScriptedClock script{.StepCost = 0.004};
    constexpr f64 Budget = 0.010;

    const SimStep step = clock.Run(Step60 * 5.5f, script.Step(), script.Clock());

    // The frame stops at the first step whose completion passes the budget, not at the clamp.
    REQUIRE(step.Steps < 5);
    REQUIRE(script.Steps.size() == step.Steps);
    CHECK(script.StartedAt.back() < Budget);
    CHECK(script.Now >= Budget);
    CHECK(script.Steps.back().Last);
    CHECK(step.Clamped);
    CHECK(step.BudgetLimited);
    CHECK(step.Alpha == doctest::Approx(0.0f));

    // The frame's final two steps recorded their poses, so interpolation reads a recorded pair.
    REQUIRE(script.Steps.size() >= 2);
    CHECK(script.Steps[script.Steps.size() - 1].RecordsHistory);
    CHECK(script.Steps[script.Steps.size() - 2].RecordsHistory);

    // Nothing is lost or invented: the steps run and the time dropped account for the frame delta.
    CHECK(static_cast<f32>(step.Steps) * Step60 + step.DroppedSeconds ==
          doctest::Approx(Step60 * 5.5f));
    CHECK(clock.GetTick() == step.Steps);

    // The dropped backlog does not resurface: a one-step frame within the budget runs one tick.
    const SimStep next = clock.Run(Step60, script.Step(), script.Clock());
    CHECK(next.Steps == 1);
    CHECK_FALSE(next.Clamped);
    CHECK(next.DroppedSeconds == doctest::Approx(0.0f));
}

TEST_CASE("sim clock: a step costing more than the budget settles at one step a frame")
{
    SimClock clock(
        SimClockInfo{.TickRate = 60, .MaxTicksPerFrame = 5, .MaxSimMillisecondsPerFrame = 10.0f});
    ScriptedClock script{.StepCost = 0.020};

    // The first frame has no step cost to predict from; every later one does.
    clock.Run(Step60 * 5.5f, script.Step(), script.Clock());
    for (int frame = 0; frame < 3; ++frame)
    {
        script.Steps.clear();
        const SimStep step = clock.Run(Step60 * 3.5f, script.Step(), script.Clock());
        CHECK(step.Steps == 1);
        CHECK(step.BudgetLimited);
        REQUIRE(script.Steps.size() == 1);
        CHECK(script.Steps[0].Last);
        CHECK(script.Steps[0].RecordsHistory);
    }
}

TEST_CASE("sim clock: a run its caller ends early keeps the backlog")
{
    SimClock clock = Make();

    const SimStep step =
        clock.Run(Step60 * 3.5f, [](const SimStepInfo&) { return false; }, [] { return 0.0; });
    CHECK(step.Steps == 1);
    CHECK_FALSE(step.Clamped);
    CHECK(step.DroppedSeconds == doctest::Approx(0.0f));

    // The two steps owed are still owed.
    CHECK(clock.Advance(0.0f).Steps == 2);
}
