// The generated-texture service's adaptive budget, driven by synthetic frames with no device: the
// period it reads, the headroom it fills, the share and ceiling that bound it, the damping that
// lets it rise slowly and fall at once, and the fallbacks — no GPU timing, a driven clock, no work.
//
// GeneratedTextureBudgetController is renderer-internal (engine/src is on this target's include
// path) and a pure function of the readings it is handed, so every claim is checked here rather
// than through a GPU case that would only observe it through a timestamp pool.

#include <vector>

#include <doctest/doctest.h>

#include <Veng/Renderer/DynamicResolution.h>

#include "Renderer/GeneratedTextureBudget.h"

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr u32 Floor = GeneratedTextureService::DefaultCostBudget;

    // A frame capped at `hz` whose GPU work outside the pump takes `otherMs`.
    GeneratedTextureBudgetInput TimedFrame(const u32 hz, const f32 otherMs)
    {
        return {
            .Hint = {.FrameCapHz = hz},
            .TimingSupported = true,
            .GpuFrameMs = otherMs,
            .HasWork = true,
        };
    }

    // A controller that has learned a pump costs `msPerCost` per unit of cost.
    GeneratedTextureBudgetController Learned(const f32 msPerCost)
    {
        GeneratedTextureBudgetController controller(Floor);
        controller.Observe(msPerCost * 8.0f, 8);
        return controller;
    }

    // Resolves `frames` frames of `input` and returns the last budget.
    u32 Settle(GeneratedTextureBudgetController& controller,
               const GeneratedTextureBudgetInput& input,
               const GeneratedTextureBudgetSettings& settings, const u32 frames = 200)
    {
        u32 budget = 0;
        for (u32 i = 0; i < frames; i++)
        {
            budget = controller.Resolve(input, settings);
        }
        return budget;
    }

    // The summed cost `frames` frames of `input` spend on a device without GPU timing.
    u64 FallbackSpend(const u32 hz, const GeneratedTextureBudgetSettings& settings,
                      const u32 frames)
    {
        GeneratedTextureBudgetController controller(Floor);
        const GeneratedTextureBudgetInput input{
            .Hint = {.FrameCapHz = hz},
            .TimingSupported = false,
            .HasWork = true,
        };
        u64 total = 0;
        u32 lowest = ~0u;
        for (u32 i = 0; i < frames; i++)
        {
            const u32 budget = controller.Resolve(input, settings);
            total += budget;
            lowest = std::min(lowest, budget);
        }
        CHECK(lowest >= Floor);
        return total;
    }
}

TEST_CASE("generated texture budget: without GPU timing the spend follows the frame period")
{
    constexpr u32 Frames = 120;
    const GeneratedTextureBudgetSettings defaults{};

    // At the default reference rate a 30 Hz frame spends twice what a 60 Hz one does, so both bake
    // at the same wall-clock rate; a 120 Hz frame would want half the 60 Hz spend, which is below
    // the floor, so it holds at the floor.
    const u64 at30 = FallbackSpend(30, defaults, Frames);
    const u64 at60 = FallbackSpend(60, defaults, Frames);
    const u64 at120 = FallbackSpend(120, defaults, Frames);
    CHECK(static_cast<f64>(at60) == doctest::Approx(Floor * Frames).epsilon(0.01));
    CHECK(static_cast<f64>(at30) == doctest::Approx(2.0 * static_cast<f64>(at60)).epsilon(0.01));
    CHECK(at120 == u64{Floor} * Frames);

    // Referenced to 120 Hz the scaling shows on both sides: 120 Hz spends half what 60 Hz does, and
    // 240 Hz, which would want half of that, still spends the floor.
    const GeneratedTextureBudgetSettings fast{.FallbackReferenceHz = 120.0f};
    const u64 fast60 = FallbackSpend(60, fast, Frames);
    const u64 fast120 = FallbackSpend(120, fast, Frames);
    CHECK(static_cast<f64>(fast120 * 2) == doctest::Approx(static_cast<f64>(fast60)).epsilon(0.01));
    CHECK(static_cast<f64>(fast120) == doctest::Approx(Floor * Frames).epsilon(0.01));
    CHECK(FallbackSpend(240, fast, Frames) == u64{Floor} * Frames);

    // A 45 Hz frame wants 5.33 a frame; the carried fraction keeps the average exact.
    const u64 at45 = FallbackSpend(45, defaults, Frames);
    CHECK(static_cast<f64>(at45) / Frames == doctest::Approx(Floor * 60.0 / 45.0).epsilon(0.01));
}

TEST_CASE("generated texture budget: ample headroom raises the budget within the share and cap")
{
    constexpr f32 MsPerCost = 0.125f;
    const GeneratedTextureBudgetSettings settings{};
    GeneratedTextureBudgetController controller = Learned(MsPerCost);

    // 60 Hz with 4 ms of other GPU work: 16.67 * 0.9 - 4 = 11 ms idle, of which half is 5.5 ms.
    const GeneratedTextureBudgetInput input = TimedFrame(60, 4.0f);
    const u32 budget = Settle(controller, input, settings);
    const f32 headroom = ((1000.0f / 60.0f) * (1.0f - settings.FrameMargin)) - 4.0f;
    CHECK(budget > Floor);
    CHECK(static_cast<f32>(budget) * MsPerCost <= headroom * settings.IdleShare + 1e-3f);
    CHECK(static_cast<f32>(budget) * MsPerCost <= settings.MaxMilliseconds);
    // It fills what it is allowed to, to within one unit of cost.
    CHECK(static_cast<f32>(budget + 1) * MsPerCost > headroom * settings.IdleShare);

    // A 30 Hz cap has more idle time, so the ceiling binds instead of the share.
    GeneratedTextureBudgetController capped = Learned(MsPerCost);
    const u32 at30 = Settle(capped, TimedFrame(30, 4.0f), settings);
    CHECK(at30 > budget);
    CHECK(static_cast<f32>(at30) * MsPerCost <= settings.MaxMilliseconds + 1e-3f);
    CHECK(static_cast<f32>(at30 + 1) * MsPerCost > settings.MaxMilliseconds);
}

TEST_CASE("generated texture budget: no headroom holds the floor")
{
    const GeneratedTextureBudgetSettings settings{};
    GeneratedTextureBudgetController controller = Learned(0.05f);

    // The rest of the frame already takes the whole period, and more.
    CHECK(Settle(controller, TimedFrame(60, 1000.0f / 60.0f), settings) == Floor);
    CHECK(Settle(controller, TimedFrame(60, 40.0f), settings) == Floor);

    // A pump dearer per cost than the ceiling allows still spends the floor.
    GeneratedTextureBudgetController dear = Learned(10.0f);
    CHECK(Settle(dear, TimedFrame(30, 1.0f), settings) == Floor);
}

TEST_CASE(
    "generated texture budget: a loss of headroom drops it at once, a gain raises it gradually")
{
    const GeneratedTextureBudgetSettings settings{};
    GeneratedTextureBudgetController controller = Learned(0.05f);

    const u32 high = Settle(controller, TimedFrame(60, 2.0f), settings);
    REQUIRE(high > Floor * 4);

    // The frame's own work suddenly fills the period: the next frame is back at the floor.
    CHECK(controller.Resolve(TimedFrame(60, 16.0f), settings) == Floor);

    // Headroom returns: the first frame takes a step, not the whole gap, and the budget climbs
    // monotonically back to where it settled before.
    const u32 first = controller.Resolve(TimedFrame(60, 2.0f), settings);
    CHECK(first > Floor);
    CHECK(first < high);
    u32 previous = first;
    bool monotone = true;
    u32 frames = 1;
    while (previous < high && frames < 200)
    {
        const u32 next = controller.Resolve(TimedFrame(60, 2.0f), settings);
        monotone = monotone && next >= previous;
        previous = next;
        frames++;
    }
    CHECK(monotone);
    CHECK(previous == high);
    CHECK(frames > 4);
}

TEST_CASE("generated texture budget: no work, a driven clock, a disabled policy and no pacing "
          "spend the floor")
{
    const GeneratedTextureBudgetSettings settings{};
    GeneratedTextureBudgetController controller = Learned(0.05f);
    REQUIRE(Settle(controller, TimedFrame(60, 2.0f), settings) > Floor);

    // Nothing queued: no boost, and none carried into the next job's first frame.
    GeneratedTextureBudgetInput idle = TimedFrame(60, 2.0f);
    idle.HasWork = false;
    CHECK(controller.Resolve(idle, settings) == Floor);
    const u32 resumed = controller.Resolve(TimedFrame(60, 2.0f), settings);
    CHECK(resumed < Settle(controller, TimedFrame(60, 2.0f), settings));

    GeneratedTextureBudgetInput driven = TimedFrame(30, 2.0f);
    driven.Hint.Driven = true;
    CHECK(Settle(controller, driven, settings, 8) == Floor);
    GeneratedTextureBudgetInput drivenUntimed = driven;
    drivenUntimed.TimingSupported = false;
    CHECK(Settle(controller, drivenUntimed, settings, 8) == Floor);

    GeneratedTextureBudgetSettings disabled = settings;
    disabled.Enabled = false;
    CHECK(Settle(controller, TimedFrame(30, 2.0f), disabled, 8) == Floor);

    // A hint naming no period — a context nothing paces — keeps the fixed floor too.
    GeneratedTextureBudgetController unpaced = Learned(0.05f);
    const GeneratedTextureBudgetInput none = TimedFrame(0, 2.0f);
    CHECK(Settle(unpaced, none, settings, 8) == Floor);
}

TEST_CASE("generated texture budget: the period is the cap's, else the smoothed measured delta")
{
    GeneratedTextureBudgetController controller(Floor);
    const GeneratedTextureBudgetSettings settings{};

    // A cap names its period whatever was measured.
    GeneratedTextureBudgetInput capped = TimedFrame(30, 0.0f);
    capped.Hint.MeasuredDeltaSeconds = 1.0f / 144.0f;
    (void)controller.Resolve(capped, settings);
    CHECK(controller.GetPeriodMs() == doctest::Approx(1000.0f / 30.0f));

    // Uncapped, the measured delta converges, and one hitch moves it a little rather than to the
    // hitch's length.
    GeneratedTextureBudgetInput uncapped = TimedFrame(0, 0.0f);
    uncapped.Hint.MeasuredDeltaSeconds = 1.0f / 60.0f;
    Settle(controller, uncapped, settings, 200);
    CHECK(controller.GetPeriodMs() == doctest::Approx(1000.0f / 60.0f).epsilon(0.01));
    uncapped.Hint.MeasuredDeltaSeconds = 2.0f;
    (void)controller.Resolve(uncapped, settings);
    CHECK(controller.GetPeriodMs() < 30.0f);
    CHECK(controller.GetPeriodMs() <= GeneratedTextureBudgetController::MaxPeriodMs);
}

TEST_CASE("generated texture budget: a pump's cost is learned from the frames that measured one")
{
    GeneratedTextureBudgetController controller(Floor);
    CHECK(controller.GetMillisecondsPerCost() == 0.0f);

    // A frame that spent nothing, or whose pump went unmeasured, teaches nothing.
    controller.Observe(3.0f, 0);
    controller.Observe(0.0f, 12);
    CHECK(controller.GetMillisecondsPerCost() == 0.0f);

    // Until it has learned, a timed frame spends the floor.
    CHECK(controller.Resolve(TimedFrame(30, 1.0f), {}) == Floor);

    controller.Observe(2.0f, 8);
    CHECK(controller.GetMillisecondsPerCost() == doctest::Approx(0.25f));
    for (u32 i = 0; i < 64; i++)
    {
        controller.Observe(1.0f, 10);
    }
    CHECK(controller.GetMillisecondsPerCost() == doctest::Approx(0.1f).epsilon(0.01));
}

TEST_CASE("generated texture budget: dynamic resolution reacts to the frame less the pump scope")
{
    const std::vector<Context::GpuPassTiming> timings = {
        {.Name = "Frame Acquires", .Milliseconds = 0.1f, .Depth = 0},
        {.Name = string(GeneratedTextureService::GpuScopeName), .Milliseconds = 6.0f, .Depth = 0},
        {.Name = "Sky Bake", .Milliseconds = 5.5f, .Depth = 1},
        {.Name = "Scene GBuffer", .Milliseconds = 4.0f, .Depth = 0},
    };
    const f32 pumpMs = FindTopLevelGpuScopeMs(timings, GeneratedTextureService::GpuScopeName);
    CHECK(pumpMs == doctest::Approx(6.0f));
    CHECK(FindTopLevelGpuScopeMs(timings, "Sky Bake") == 0.0f);
    CHECK(FindTopLevelGpuScopeMs({}, GeneratedTextureService::GpuScopeName) == 0.0f);

    // The frame measured 16 ms against a 15 ms budget, but 6 of them were background generation:
    // the controller sees 10 ms and does not lower the scale.
    const DynamicResolutionSettings settings{.TargetFrameTimeMs = 15.0f, .MinScale = 0.5f};
    const f32 seen = DynamicResolutionFrameTimeMs(16.0f, pumpMs);
    CHECK(seen == doctest::Approx(10.0f));
    CHECK(ComputeDynamicResolutionScale(1.0f, seen, settings) == doctest::Approx(1.0f));
    CHECK(ComputeDynamicResolutionScale(1.0f, 16.0f, settings) < 1.0f);

    // No measurement stays no measurement, and a frame of nothing but the pump reads as light.
    CHECK(DynamicResolutionFrameTimeMs(0.0f, 3.0f) == 0.0f);
    CHECK(DynamicResolutionFrameTimeMs(5.0f, 7.0f) > 0.0f);
}
