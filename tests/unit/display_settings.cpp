// Built-in display settings, device-free:
//
//  - the capability-query parsing half: DedupVideoModes sorts and collapses a monitor's raw mode
//    list (the GLFW-backed enumeration itself is smoke-tier — it needs a window);
//  - present-mode fallback: an unsupported request resolves to Vsync, never a hard failure;
//  - the per-platform fullscreen set: PlatformFullscreenModes offers Windowed + a single native
//    fullscreen (Borderless) on macOS, and adds Exclusive elsewhere;
//  - invalid-selection fallback against a stubbed capability set: a persisted monitor / resolution /
//    refresh the hardware no longer offers resolves to a supported one, an exclusive mode a platform
//    does not offer drops to its fullscreen choice, and a three-way platform keeps exclusive intact;
//  - the diffed apply: a frame-cap-only change asks for no swapchain work; a resolution change asks
//    for exactly the window (swapchain-recreating) action;
//  - the frame-rate limiter spaces frames to the requested rate against a synthetic clock and is a
//    no-op at 0.
//
// No Context, window, or GLFW is touched: every function under test is pure.

#include <doctest/doctest.h>

#include <algorithm>
#include <array>

#include <Veng/Render/DisplayCapabilities.h>
#include <Veng/Render/FrameRateLimiter.h>
#include <Veng/Render/GraphicsSettings.h>

#include "Render/DisplayResolve.h"

using namespace Veng;

namespace
{
    // A two-monitor capability set: the primary offers 1080p@{60,120} and 720p@60; the secondary
    // offers 4K@60. Exclusive fullscreen is unsupported, mirroring the macOS dev host.
    DisplayCapabilities StubCaps()
    {
        DisplayCapabilities caps;
        caps.PresentModes = {PresentMode::Vsync, PresentMode::Immediate};
        // The macOS dev host: full-screen is a single native toggle (no Exclusive).
        caps.AvailableFullscreenModes = {FullscreenMode::Windowed, FullscreenMode::Borderless};

        MonitorInfo primary;
        primary.MonitorId = 0;
        primary.Name = "Primary";
        primary.CurrentResolution = {1920, 1080};
        primary.CurrentRefreshRateHz = 60;
        primary.Modes = {DisplayVideoMode{.Resolution = {1280, 720}, .RefreshRateHz = 60},
                         DisplayVideoMode{.Resolution = {1920, 1080}, .RefreshRateHz = 60},
                         DisplayVideoMode{.Resolution = {1920, 1080}, .RefreshRateHz = 120}};

        MonitorInfo secondary;
        secondary.MonitorId = 1;
        secondary.Name = "Secondary";
        secondary.CurrentResolution = {3840, 2160};
        secondary.CurrentRefreshRateHz = 60;
        secondary.Modes = {DisplayVideoMode{.Resolution = {3840, 2160}, .RefreshRateHz = 60}};

        caps.Monitors = {primary, secondary};
        return caps;
    }
}

TEST_CASE("DedupVideoModes sorts by area then refresh and collapses duplicates")
{
    const std::array<DisplayVideoMode, 5> raw{
        DisplayVideoMode{.Resolution = {1920, 1080}, .RefreshRateHz = 60},
        DisplayVideoMode{.Resolution = {1280, 720}, .RefreshRateHz = 60},
        DisplayVideoMode{.Resolution = {1920, 1080}, .RefreshRateHz = 120},
        // Exact duplicate of the first (GLFW reports one per bit-depth) — must collapse.
        DisplayVideoMode{.Resolution = {1920, 1080}, .RefreshRateHz = 60},
        DisplayVideoMode{.Resolution = {1280, 720}, .RefreshRateHz = 60},
    };

    const vector<DisplayVideoMode> modes = DedupVideoModes(raw);

    REQUIRE(modes.size() == 3);
    // Ascending by area: 720p then the two 1080p modes; equal area orders by refresh.
    CHECK(modes[0].Resolution == uvec2{1280, 720});
    CHECK(modes[1].Resolution == uvec2{1920, 1080});
    CHECK(modes[1].RefreshRateHz == 60);
    CHECK(modes[2].Resolution == uvec2{1920, 1080});
    CHECK(modes[2].RefreshRateHz == 120);
}

TEST_CASE("ResolvePresentMode falls back to Vsync for an unsupported request")
{
    const std::array<PresentMode, 2> supported{PresentMode::Vsync, PresentMode::Immediate};

    // A supported request passes through.
    CHECK(ResolvePresentMode(PresentMode::Immediate, supported) == PresentMode::Immediate);
    // An unsupported request resolves to Vsync (FIFO), never a failure.
    CHECK(ResolvePresentMode(PresentMode::Mailbox, supported) == PresentMode::Vsync);
    // Vsync is always supported.
    CHECK(ResolvePresentMode(PresentMode::Vsync, supported) == PresentMode::Vsync);
}

TEST_CASE("ResolveDisplaySelection clamps a selection naming gone hardware")
{
    const DisplayCapabilities caps = StubCaps();

    SUBCASE("an absent monitor falls back to the primary")
    {
        BuiltinDisplayChoices sel;
        sel.MonitorId = 7;
        const BuiltinDisplayChoices out = ResolveDisplaySelection(sel, caps);
        CHECK(out.MonitorId == 0);
    }

    SUBCASE("a resolution the monitor cannot drive clamps to the nearest offered")
    {
        BuiltinDisplayChoices sel;
        sel.MonitorId = 0;
        sel.Resolution = {2560, 1440}; // not offered; nearest by area is 1080p
        const BuiltinDisplayChoices out = ResolveDisplaySelection(sel, caps);
        CHECK(out.Resolution == uvec2{1920, 1080});
    }

    SUBCASE("a native (zero) resolution is left alone")
    {
        BuiltinDisplayChoices sel;
        sel.Resolution = {0, 0};
        const BuiltinDisplayChoices out = ResolveDisplaySelection(sel, caps);
        CHECK(out.Resolution == uvec2{0, 0});
    }

    SUBCASE("a refresh the resolution cannot drive clamps to the highest offered there")
    {
        BuiltinDisplayChoices sel;
        sel.MonitorId = 0;
        sel.Resolution = {1920, 1080};
        sel.RefreshRateHz = 240; // 1080p offers 60 and 120; clamp to 120
        const BuiltinDisplayChoices out = ResolveDisplaySelection(sel, caps);
        CHECK(out.RefreshRateHz == 120);
    }

    SUBCASE("an exclusive mode the platform does not offer drops to the fullscreen choice")
    {
        // caps offers only {Windowed, Borderless} (the macOS set), so Exclusive clamps to Borderless.
        BuiltinDisplayChoices sel;
        sel.Fullscreen = FullscreenMode::Exclusive;
        const BuiltinDisplayChoices out = ResolveDisplaySelection(sel, caps);
        CHECK(out.Fullscreen == FullscreenMode::Borderless);
    }

    SUBCASE("a three-way platform leaves an exclusive selection intact")
    {
        DisplayCapabilities threeWay = caps;
        threeWay.AvailableFullscreenModes = {FullscreenMode::Windowed, FullscreenMode::Borderless,
                                             FullscreenMode::Exclusive};
        BuiltinDisplayChoices sel;
        sel.Fullscreen = FullscreenMode::Exclusive;
        const BuiltinDisplayChoices out = ResolveDisplaySelection(sel, threeWay);
        CHECK(out.Fullscreen == FullscreenMode::Exclusive);
    }

    SUBCASE("an unsupported present mode drops to Vsync")
    {
        BuiltinDisplayChoices sel;
        sel.Present = PresentMode::Mailbox; // caps offer Vsync/Immediate only
        const BuiltinDisplayChoices out = ResolveDisplaySelection(sel, caps);
        CHECK(out.Present == PresentMode::Vsync);
    }

    SUBCASE("a fully valid selection passes through unchanged")
    {
        BuiltinDisplayChoices sel;
        sel.MonitorId = 1;
        sel.Resolution = {3840, 2160};
        sel.RefreshRateHz = 60;
        sel.Present = PresentMode::Immediate;
        sel.Fullscreen = FullscreenMode::Borderless;
        const BuiltinDisplayChoices out = ResolveDisplaySelection(sel, caps);
        CHECK(out.MonitorId == 1);
        CHECK(out.Resolution == uvec2{3840, 2160});
        CHECK(out.RefreshRateHz == 60);
        CHECK(out.Present == PresentMode::Immediate);
        CHECK(out.Fullscreen == FullscreenMode::Borderless);
    }
}

TEST_CASE("PlatformFullscreenModes reports the platform's fullscreen set")
{
    const vector<FullscreenMode> modes = PlatformFullscreenModes();

    // Windowed and a single native fullscreen choice (Borderless) are offered everywhere.
    CHECK(std::ranges::find(modes, FullscreenMode::Windowed) != modes.end());
    CHECK(std::ranges::find(modes, FullscreenMode::Borderless) != modes.end());

    const bool offersExclusive = std::ranges::find(modes, FullscreenMode::Exclusive) != modes.end();
#if defined(__APPLE__)
    // macOS full-screen is one native Cocoa toggle; Exclusive is meaningless there.
    CHECK_FALSE(offersExclusive);
    CHECK(modes.size() == 2);
#else
    CHECK(offersExclusive);
    CHECK(modes.size() == 3);
#endif
}

TEST_CASE("ComputeDisplayApplyActions performs only the changes that moved")
{
    BuiltinDisplayChoices current;
    current.Resolution = {1920, 1080};
    current.Present = PresentMode::Vsync;
    current.FrameCapHz = 0;

    SUBCASE("a frame-cap-only change touches no swapchain")
    {
        BuiltinDisplayChoices next = current;
        next.FrameCapHz = 60;
        const DisplayApplyActions actions = ComputeDisplayApplyActions(current, next);
        CHECK(actions.ChangeFrameCap);
        CHECK_FALSE(actions.ChangeWindow);
        CHECK_FALSE(actions.ChangePresentMode);
    }

    SUBCASE("a resolution change asks for exactly the window action")
    {
        BuiltinDisplayChoices next = current;
        next.Resolution = {1280, 720};
        const DisplayApplyActions actions = ComputeDisplayApplyActions(current, next);
        CHECK(actions.ChangeWindow);
        CHECK_FALSE(actions.ChangePresentMode);
        CHECK_FALSE(actions.ChangeFrameCap);
    }

    SUBCASE("a present-mode change asks for the present action only")
    {
        BuiltinDisplayChoices next = current;
        next.Present = PresentMode::Immediate;
        const DisplayApplyActions actions = ComputeDisplayApplyActions(current, next);
        CHECK(actions.ChangePresentMode);
        CHECK_FALSE(actions.ChangeWindow);
        CHECK_FALSE(actions.ChangeFrameCap);
    }

    SUBCASE("no change asks for nothing")
    {
        const DisplayApplyActions actions = ComputeDisplayApplyActions(current, current);
        CHECK_FALSE(actions.ChangeWindow);
        CHECK_FALSE(actions.ChangePresentMode);
        CHECK_FALSE(actions.ChangeFrameCap);
    }

    SUBCASE("the first apply forces present and frame cap but not a default window")
    {
        // Windowed, native resolution/refresh, primary monitor.
        const BuiltinDisplayChoices defaults;
        const DisplayApplyActions actions = ComputeDisplayApplyActions(std::nullopt, defaults);
        CHECK(actions.ChangePresentMode);
        CHECK(actions.ChangeFrameCap);
        CHECK_FALSE(actions.ChangeWindow);
    }

    SUBCASE("the first apply of a non-default window does touch it")
    {
        BuiltinDisplayChoices sel;
        sel.Fullscreen = FullscreenMode::Borderless;
        const DisplayApplyActions actions = ComputeDisplayApplyActions(std::nullopt, sel);
        CHECK(actions.ChangeWindow);
    }
}

TEST_CASE("DecideFullscreenWriteBack catches a user toggle and ignores the engine's own apply")
{
    using Veng::DecideFullscreenWriteBack;
    constexpr FullscreenMode Windowed = FullscreenMode::Windowed;
    constexpr FullscreenMode Full = FullscreenMode::Borderless;

    SUBCASE("the seed frame writes nothing")
    {
        // No prior observation: whatever the live mode, there is no change to attribute yet.
        CHECK_FALSE(DecideFullscreenWriteBack(Full, std::nullopt, Windowed).has_value());
    }

    SUBCASE("no change since last frame writes nothing")
    {
        CHECK_FALSE(DecideFullscreenWriteBack(Windowed, Windowed, Windowed).has_value());
        CHECK_FALSE(DecideFullscreenWriteBack(Full, Full, Full).has_value());
    }

    SUBCASE("a user toggle into and out of fullscreen is written back")
    {
        // The window changed to a mode the engine did not apply (applied is still the old one): the
        // green-button toggle, in both directions.
        const optional<FullscreenMode> intoFull =
            DecideFullscreenWriteBack(Full, Windowed, Windowed);
        REQUIRE(intoFull.has_value());
        CHECK(*intoFull == Full);

        const optional<FullscreenMode> outOfFull = DecideFullscreenWriteBack(Windowed, Full, Full);
        REQUIRE(outOfFull.has_value());
        CHECK(*outOfFull == Windowed);
    }

    SUBCASE("the engine's own async apply completing writes nothing")
    {
        // The engine applied Full; the window was still Windowed (async) and now reaches Full. The
        // change matches what was applied, so it is that apply landing — the store already holds it.
        CHECK_FALSE(DecideFullscreenWriteBack(Full, Windowed, Full).has_value());
    }
}

TEST_CASE("FrameRateLimiter spaces frames to the requested rate")
{
    FrameRateLimiter limiter;

    SUBCASE("uncapped is always a no-op")
    {
        CHECK(limiter.GetCapHz() == 0);
        CHECK(limiter.AcquireSleepSeconds(0.0) == doctest::Approx(0.0));
        CHECK(limiter.AcquireSleepSeconds(100.0) == doctest::Approx(0.0));
    }

    SUBCASE("a 60 Hz cap holds the frame interval near 1/60 s against a synthetic clock")
    {
        limiter.SetCapHz(60);
        const f64 period = 1.0 / 60.0;

        // Simulate a loop whose own work is instantaneous: each frame we advance the clock by the
        // sleep the limiter returned, so the achieved interval is exactly the sleep.
        f64 now = 1000.0;
        f64 minInterval = 1.0;
        f64 maxInterval = 0.0;
        for (int i = 0; i < 200; ++i)
        {
            const f64 sleep = limiter.AcquireSleepSeconds(now);
            now += sleep;
            if (i > 0)
            {
                minInterval = std::min(minInterval, sleep);
                maxInterval = std::max(maxInterval, sleep);
            }
        }
        CHECK(minInterval == doctest::Approx(period).epsilon(0.01));
        CHECK(maxInterval == doctest::Approx(period).epsilon(0.01));
    }

    SUBCASE("a frame that overran the deadline is not clawed back")
    {
        limiter.SetCapHz(60);
        const f64 period = 1.0 / 60.0;
        // Arm.
        CHECK(limiter.AcquireSleepSeconds(0.0) == doctest::Approx(period));
        // The next frame arrives far past its deadline: no sleep, and no accumulated debt.
        CHECK(limiter.AcquireSleepSeconds(10.0) == doctest::Approx(0.0));
        // The following frame is paced from the caught-up deadline, not from an old backlog.
        const f64 sleep = limiter.AcquireSleepSeconds(10.0);
        CHECK(sleep == doctest::Approx(period));
    }

    SUBCASE("setting the cap to 0 disables it")
    {
        limiter.SetCapHz(120);
        CHECK(limiter.AcquireSleepSeconds(0.0) > 0.0);
        limiter.SetCapHz(0);
        CHECK(limiter.AcquireSleepSeconds(1.0) == doctest::Approx(0.0));
    }
}
