// Client tick-offset controller cases. ClockSync is pure and device-free: TargetTickOffset folds a
// smoothed RTT/jitter into the lead the client should run ahead of the server, SlewForOffsetError
// turns a lead error into a bounded step multiplier, and TickOffsetEstimator smooths a raw RTT
// stream and holds the running estimate. These pin the math and the controller over scripted
// RTT/jitter traces — target arithmetic, slew bounds, convergence, a mid-trace step change, the
// closed-loop feedback trim, and the no-double-run/no-skip invariant — with no socket and no clock.

#include <doctest/doctest.h>

#include <Veng/Net/ClockSync.h>

#include <cmath>

using namespace Veng;
using namespace Veng::Net;

namespace
{
    // 60 Hz sim, 1-tick margin, ±5% slew, 0.2 tick-error gain — the controller defaults.
    constexpr TickSyncSettings Settings{
        .TickRate = 60, .MarginTicks = 1.0f, .MaxSlew = 0.05f, .SlewGain = 0.2f};
}

TEST_CASE("clock sync: target lead is (RTT/2 + jitter)·rate + margin, clamped at zero")
{
    // rtt 0.1 s ⇒ 0.05 s one-way, jitter 0.02 s ⇒ 0.07 s lead · 60 = 4.2 ticks, + 1 margin = 5.2.
    CHECK(TargetTickOffset(0.1f, 0.02f, 0.0f, Settings) == doctest::Approx(5.2f));

    // Zero link state still leads by the margin.
    CHECK(TargetTickOffset(0.0f, 0.0f, 0.0f, Settings) == doctest::Approx(1.0f));

    // A feedback trim larger than the lead clamps the target at zero, never negative.
    CHECK(TargetTickOffset(0.1f, 0.02f, 100.0f, Settings) == doctest::Approx(0.0f));
}

TEST_CASE("clock sync: the slew is bounded and directed by the lead error")
{
    // No error ⇒ no slew.
    CHECK(SlewForOffsetError(0.0f, Settings) == doctest::Approx(1.0f));

    // A small positive error (client behind the target lead) speeds it up, proportionally.
    CHECK(SlewForOffsetError(0.1f, Settings) == doctest::Approx(1.02f));

    // A small negative error slows it down.
    CHECK(SlewForOffsetError(-0.1f, Settings) == doctest::Approx(0.98f));

    // A large error saturates at the ± bound — never a hard step that doubles or skips a tick.
    CHECK(SlewForOffsetError(50.0f, Settings) == doctest::Approx(1.05f));
    CHECK(SlewForOffsetError(-50.0f, Settings) == doctest::Approx(0.95f));
}

TEST_CASE("clock sync: the controller converges the client lead to the target and stays bounded")
{
    // Model the client tick as a real position advancing by the slew each server tick; the server
    // advances by one. The lead (clientPos - serverTick) must approach the target and hold there,
    // the slew never leaving its bound and the client never stalling or double-advancing.
    const f32 rtt = 0.1f;
    const f32 jitter = 0.02f;
    const f32 target = TargetTickOffset(rtt, jitter, 0.0f, Settings);

    f32 clientPos = 0.0f;
    u64 serverTick = 0;
    for (int step = 0; step < 600; ++step)
    {
        const f32 offset = clientPos - static_cast<f32>(serverTick);
        const TickOffsetEstimate est = EstimateTickOffset(
            TickOffsetInput{
                .RttSeconds = rtt, .JitterSeconds = jitter, .CurrentOffsetTicks = offset},
            Settings);

        CHECK(est.SlewFactor >= 1.0f - Settings.MaxSlew);
        CHECK(est.SlewFactor <= 1.0f + Settings.MaxSlew);

        const f32 previous = clientPos;
        clientPos += est.SlewFactor;
        ++serverTick;

        // No skip (the client always advances) and no double-run (never a whole extra tick).
        CHECK(clientPos > previous);
        CHECK((clientPos - previous) < 2.0f);
    }

    const f32 settledLead = clientPos - static_cast<f32>(serverTick);
    CHECK(settledLead == doctest::Approx(target).epsilon(0.02));
}

TEST_CASE("clock sync: a mid-trace RTT step change re-converges the lead")
{
    f32 rtt = 0.05f;
    const f32 jitter = 0.0f;

    f32 clientPos = 0.0f;
    u64 serverTick = 0;
    const auto run = [&](int steps)
    {
        for (int step = 0; step < steps; ++step)
        {
            const f32 offset = clientPos - static_cast<f32>(serverTick);
            clientPos += EstimateTickOffset(TickOffsetInput{.RttSeconds = rtt,
                                                            .JitterSeconds = jitter,
                                                            .CurrentOffsetTicks = offset},
                                            Settings)
                             .SlewFactor;
            ++serverTick;
        }
    };

    run(400);
    CHECK((clientPos - static_cast<f32>(serverTick)) ==
          doctest::Approx(TargetTickOffset(rtt, jitter, 0.0f, Settings)).epsilon(0.02));

    // The link degrades: RTT quadruples. The lead must climb to the new, larger target.
    rtt = 0.2f;
    run(600);
    CHECK((clientPos - static_cast<f32>(serverTick)) ==
          doctest::Approx(TargetTickOffset(rtt, jitter, 0.0f, Settings)).epsilon(0.02));
}

TEST_CASE("clock sync: the estimator seeds on the first sample and grows jitter from RTT variation")
{
    TickOffsetEstimator estimator(Settings);
    CHECK_FALSE(estimator.HasEstimate());

    // The first observation seeds the smoothed RTT exactly, with zero jitter.
    estimator.Observe(0.1f, 10, 5);
    CHECK(estimator.HasEstimate());
    CHECK(estimator.SmoothedRtt() == doctest::Approx(0.1f));
    CHECK(estimator.SmoothedJitter() == doctest::Approx(0.0f));

    // A steady RTT keeps jitter near zero.
    for (int i = 0; i < 20; ++i)
    {
        estimator.Observe(0.1f, 10, 5);
    }
    CHECK(estimator.SmoothedJitter() < 0.001f);

    // A varying RTT drives jitter up.
    for (int i = 0; i < 40; ++i)
    {
        estimator.Observe(i % 2 == 0 ? 0.2f : 0.05f, 10, 5);
    }
    CHECK(estimator.SmoothedJitter() > 0.01f);
}

TEST_CASE("clock sync: the estimator's target matches the pure fold and the feedback trims it")
{
    TickOffsetEstimator estimator(Settings);
    for (int i = 0; i < 50; ++i)
    {
        estimator.Observe(0.08f, 20, 12);
    }

    const f32 expected =
        TargetTickOffset(estimator.SmoothedRtt(), estimator.SmoothedJitter(), 0.0f, Settings);
    CHECK(estimator.TargetOffset() == doctest::Approx(expected));

    // A positive feedback trim (inputs arriving early) shrinks the target on the next observation.
    estimator.SetFeedbackTrim(2.0f);
    estimator.Observe(0.08f, 20, 12);
    CHECK(estimator.TargetOffset() == doctest::Approx(expected - 2.0f).epsilon(0.05));
}

TEST_CASE("clock sync: the stateful estimator's slew stays bounded across a scripted trace")
{
    TickOffsetEstimator estimator(Settings);
    f32 clientPos = 0.0f;
    u64 serverTick = 0;
    for (int step = 0; step < 400; ++step)
    {
        // A noisy RTT the estimator smooths internally.
        const f32 rtt = 0.1f + (step % 5 == 0 ? 0.03f : 0.0f);
        const f32 slew =
            estimator.Observe(rtt, static_cast<u64>(std::llround(clientPos)), serverTick);
        CHECK(slew >= 1.0f - Settings.MaxSlew);
        CHECK(slew <= 1.0f + Settings.MaxSlew);
        clientPos += slew;
        ++serverTick;
    }

    // The lead settles near the smoothed target (integer-tick offset resolution ⇒ a looser bound).
    const f32 target =
        TargetTickOffset(estimator.SmoothedRtt(), estimator.SmoothedJitter(), 0.0f, Settings);
    CHECK((clientPos - static_cast<f32>(serverTick)) == doctest::Approx(target).epsilon(0.25));
}
