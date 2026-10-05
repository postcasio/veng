// The haptics engine and its pure core: a clip's evaluation (looping and not), the per-pad mix
// (maximum per channel, intensity, the master clamp), and the instance lifecycle — retirement, a
// fading stop, a paused and a closed world, a seat following its pad, the replay gate, inspection of
// a padless seat, and output suspension. The engine runs over hooks the test supplies and clips
// adopted without a device, so it runs in the default band.

#include <doctest/doctest.h>

#include <array>
#include <unordered_map>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Haptics/Haptics.h>

using namespace Veng;
using namespace Veng::Haptics;

namespace
{
    // A clip holding every channel at one level for its whole duration.
    RumbleClipData Constant(const f32 level, const f32 duration, const bool loop = false)
    {
        const Curve1D flat{.Keys = {{.Time = 0.0f, .Value = level}}};
        return RumbleClipData{.Duration = duration,
                              .Loop = loop,
                              .LowFrequency = flat,
                              .HighFrequency = flat,
                              .LeftTrigger = flat,
                              .RightTrigger = flat};
    }

    AssetHandle<RumbleClip> Adopt(RumbleClipData data)
    {
        return AssetManager::Adopt(RumbleClip::Create(std::move(data)));
    }

    // What the engine last wrote to each pad.
    struct Motors
    {
        std::array<RumbleChannels, Input::MaxGamepads> Written{};

        HapticsEngineInfo Sink()
        {
            return HapticsEngineInfo{
                .WriteMotors = [this](const GamepadId pad, const RumbleChannels& levels)
                { Written[static_cast<usize>(pad)] = levels; }};
        }
    };

    constexpr GamepadId Pad0 = static_cast<GamepadId>(0);
    constexpr GamepadId Pad1 = static_cast<GamepadId>(1);
    constexpr GamepadId Pad3 = static_cast<GamepadId>(3);
}

TEST_CASE("haptics: a looping clip wraps and a non-looping one reads zero past its duration")
{
    const RumbleClipData clip{.Duration = 1.0f,
                              .LowFrequency = Curve1D{.Keys = {{.Time = 0.0f, .Value = 0.0f},
                                                               {.Time = 1.0f, .Value = 1.0f}}}};

    CHECK(EvaluateClip(clip, 0.25f, false).LowFrequency == doctest::Approx(0.25f));
    CHECK(EvaluateClip(clip, 1.25f, true).LowFrequency == doctest::Approx(0.25f));
    CHECK(EvaluateClip(clip, 1.25f, false) == RumbleChannels{});
    CHECK(EvaluateClip(clip, 1.0f, false) == RumbleChannels{});
    // An absent channel is silent.
    CHECK(EvaluateClip(clip, 0.5f, false).HighFrequency == 0.0f);
}

TEST_CASE("haptics: the mix takes the maximum per channel, then the master, clamped")
{
    const RumbleChannels hum{.LowFrequency = 0.3f, .HighFrequency = 0.6f};
    const RumbleChannels pulse{.LowFrequency = 0.9f, .HighFrequency = 0.1f, .RightTrigger = 0.4f};
    const RumbleChannels layers[] = {hum, pulse};

    const RumbleChannels mixed = MixRumble(layers);
    CHECK(mixed.LowFrequency == doctest::Approx(0.9f));
    CHECK(mixed.HighFrequency == doctest::Approx(0.6f));
    CHECK(mixed.LeftTrigger == 0.0f);
    CHECK(mixed.RightTrigger == doctest::Approx(0.4f));

    // Intensity scales a layer before the maximum; the master scales the result.
    const RumbleChannels scaled[] = {ScaleRumble(hum, 2.0f), pulse};
    CHECK(MixRumble(scaled).HighFrequency == doctest::Approx(1.0f));
    CHECK(MixRumble(layers, 0.5f).LowFrequency == doctest::Approx(0.45f));

    // Nothing leaves [0, 1], however hot the layers.
    const RumbleChannels hot[] = {ScaleRumble(pulse, 5.0f)};
    CHECK(MixRumble(hot).LowFrequency == 1.0f);
    CHECK(MixRumble({}) == RumbleChannels{});
}

TEST_CASE("haptics: an instance sounds from its start and a non-looping one retires")
{
    Motors motors;
    HapticsEngine engine(motors.Sink());
    const RumbleHandle handle =
        engine.Play(RumbleTarget::ForGamepad(Pad0), Adopt(Constant(0.5f, 0.25f)));
    REQUIRE(handle.IsValid());

    engine.Update({.Delta = 1.0f});
    CHECK(engine.IsPlaying(handle));
    CHECK(motors.Written[0].LowFrequency == doctest::Approx(0.5f));
    CHECK(motors.Written[1] == RumbleChannels{});

    engine.Update({.Delta = 0.3f});
    CHECK_FALSE(engine.IsPlaying(handle));
    CHECK(motors.Written[0] == RumbleChannels{});
}

TEST_CASE("haptics: a stop with a fade ramps to zero over the fade, then retires")
{
    Motors motors;
    HapticsEngine engine(motors.Sink());
    const RumbleHandle handle =
        engine.Play(RumbleTarget::ForGamepad(Pad0), Adopt(Constant(1.0f, 1.0f, true)));
    engine.Update({.Delta = 0.0f});
    CHECK(motors.Written[0].LowFrequency == doctest::Approx(1.0f));

    engine.Stop(handle, 1.0f);
    engine.Update({.Delta = 0.25f});
    CHECK(motors.Written[0].LowFrequency == doctest::Approx(0.75f));
    engine.Update({.Delta = 0.5f});
    CHECK(motors.Written[0].LowFrequency == doctest::Approx(0.25f));
    CHECK(engine.IsPlaying(handle));

    engine.Update({.Delta = 0.25f});
    CHECK_FALSE(engine.IsPlaying(handle));
    CHECK(motors.Written[0] == RumbleChannels{});

    // An immediate stop retires at once.
    const RumbleHandle again =
        engine.Play(RumbleTarget::ForGamepad(Pad0), Adopt(Constant(1.0f, 1.0f, true)));
    engine.Stop(again);
    CHECK_FALSE(engine.IsPlaying(again));
}

TEST_CASE("haptics: a paused world's instance holds and is silent; a closed world's stops")
{
    const WorldInstanceId world{.Value = 7};
    HapticsWorldState state = HapticsWorldState::Open;

    Motors motors;
    HapticsEngineInfo info = motors.Sink();
    info.WorldState = [&](const WorldInstanceId) { return state; };
    HapticsEngine engine(std::move(info));

    const RumbleTarget target = RumbleTarget::ForGamepad(Pad0);
    const RumbleHandle owned =
        engine.Play(target, Adopt(Constant(0.8f, 2.0f)), RumbleParams{.World = world});
    const RumbleHandle app = engine.Play(target, Adopt(Constant(0.2f, 10.0f)));
    engine.Update({.Delta = 0.0f});
    engine.Update({.Delta = 0.5f});

    state = HapticsWorldState::Paused;
    engine.Update({.Delta = 1.0f});
    // The application-owned hum still plays; the paused instance contributes nothing and holds.
    CHECK(motors.Written[0].LowFrequency == doctest::Approx(0.2f));
    vector<RumbleInstanceInfo> playing = engine.GetInstances(target);
    REQUIRE(playing.size() == 2);
    CHECK(playing[0].Paused);
    CHECK(playing[0].Time == doctest::Approx(0.5f));

    state = HapticsWorldState::Open;
    engine.Update({.Delta = 0.25f});
    CHECK(motors.Written[0].LowFrequency == doctest::Approx(0.8f));
    playing = engine.GetInstances(target);
    CHECK(playing[0].Time == doctest::Approx(0.75f));

    state = HapticsWorldState::Closed;
    engine.Update({.Delta = 0.0f});
    CHECK_FALSE(engine.IsPlaying(owned));
    CHECK(engine.IsPlaying(app));
}

TEST_CASE("haptics: a seat target follows its pad and is silent without one")
{
    const SeatRef seat{.World = WorldInstanceId{.Value = 1}, .Viewer = Entity{.Index = 4}};
    GamepadId assigned = Pad1;

    Motors motors;
    HapticsEngineInfo info = motors.Sink();
    info.ResolveSeat = [&](const SeatRef&) { return assigned; };
    HapticsEngine engine(std::move(info));

    const RumbleTarget target = RumbleTarget::ForSeat(seat);
    const RumbleHandle handle = engine.Play(target, Adopt(Constant(0.6f, 5.0f)));
    engine.Update({.Delta = 0.0f});
    CHECK(motors.Written[1].LowFrequency == doctest::Approx(0.6f));

    assigned = Pad3;
    engine.Update({.Delta = 0.1f});
    CHECK(motors.Written[1] == RumbleChannels{});
    CHECK(motors.Written[3].LowFrequency == doctest::Approx(0.6f));

    assigned = GamepadId::None;
    engine.Update({.Delta = 0.1f});
    CHECK(motors.Written[3] == RumbleChannels{});
    CHECK(engine.IsPlaying(handle));

    // Inspection still reports what plays on the padless seat.
    const vector<RumbleInstanceInfo> playing = engine.GetInstances(target);
    REQUIRE(playing.size() == 1);
    CHECK(playing[0].Gamepad == GamepadId::None);
    CHECK(playing[0].Handle == handle);
    CHECK(engine.GetInstances(RumbleTarget::ForGamepad(Pad3)).empty());

    engine.StopAll(target);
    CHECK_FALSE(engine.IsPlaying(handle));
}

TEST_CASE("haptics: a play during a replay, on the inert engine or of an unloaded clip starts "
          "nothing")
{
    HapticsEngine engine;
    const AssetHandle<RumbleClip> clip = Adopt(Constant(1.0f, 1.0f));
    {
        const HapticsEngine::ReplayScope replay = engine.BeginReplay();
        CHECK(engine.IsReplaying());
        CHECK_FALSE(engine.Play(RumbleTarget::ForGamepad(Pad0), clip).IsValid());
    }
    CHECK_FALSE(engine.IsReplaying());
    CHECK(engine.GetAllInstances().empty());

    CHECK(GetInertEngine().IsInert());
    CHECK_FALSE(GetInertEngine().Play(RumbleTarget::ForGamepad(Pad0), clip).IsValid());
    CHECK_FALSE(engine.Play(RumbleTarget::ForGamepad(Pad0), AssetHandle<RumbleClip>{}).IsValid());

    // Outside the scope the same play starts.
    CHECK(engine.Play(RumbleTarget::ForGamepad(Pad0), clip).IsValid());
}

TEST_CASE("haptics: a suspended output writes zero but keeps the mix and the clock")
{
    Motors motors;
    HapticsEngine engine(motors.Sink());
    engine.SetMasterIntensity(0.5f);
    const RumbleHandle handle =
        engine.Play(RumbleTarget::ForGamepad(Pad0), Adopt(Constant(1.0f, 1.0f)));
    engine.Update({.Delta = 0.0f, .OutputSuspended = true});
    CHECK(engine.IsOutputSuspended());
    CHECK(motors.Written[0] == RumbleChannels{});
    CHECK(engine.GetOutput(Pad0).LowFrequency == doctest::Approx(0.5f));

    // The pulse ends while suspended, so it does not replay when the output returns.
    engine.Update({.Delta = 2.0f, .OutputSuspended = true});
    CHECK_FALSE(engine.IsPlaying(handle));
    engine.Update({.Delta = 0.0f});
    CHECK(motors.Written[0] == RumbleChannels{});
}
