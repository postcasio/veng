// The haptics mixer and its pure core: a clip's evaluation (looping and not), the per-pad mix
// (maximum per channel, intensity, the master clamp), and what each presentation state does to the
// one-shots and layers a scope owns — Held freezes, Muted keeps time silently, Closed drops — plus the
// scoped facade's replay gate and scene-local seat resolution, output suspension, and HapticsSystem
// playing a RumbleSource through a device-free runner world. Every engine runs over a scope registry
// the case drives and clips adopted without a device, so it runs in the default band.

#include <doctest/doctest.h>

#include <array>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Haptics/Haptics.h>
#include <Veng/Haptics/HapticsSystem.h>
#include <Veng/Haptics/RumbleSource.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/PresentationScope.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SystemRegistry.h>
#include <Veng/World.h>
#include <Veng/WorldRunner.h>

#include "support/TestServices.h"

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

    // The one one-shot an engine holds; the case requires there is exactly one.
    RumbleOneShotInfo Only(const HapticsEngine& engine)
    {
        const vector<RumbleOneShotInfo> held = engine.GetOneShots();
        REQUIRE(held.size() == 1);
        return held.front();
    }

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

TEST_CASE("haptics: a one-shot sounds from its start, rises over a layer, and ends at its duration")
{
    PresentationScopes scopes;
    Motors motors;
    HapticsEngine engine(scopes, motors.Sink());
    const PresentationScopeId application = scopes.GetApplicationScope();

    engine.PlayOneShot(application, Pad0, Adopt(Constant(0.5f, 0.25f)));
    engine.SubmitLayer(application, Pad0, RumbleChannels{.LowFrequency = 0.2f});
    scopes.Resolve();
    engine.Update({.Delta = 1.0f});
    CHECK(Only(engine).Time == 0.0f);
    CHECK(motors.Written[0].LowFrequency == doctest::Approx(0.5f));
    CHECK(motors.Written[1] == RumbleChannels{});

    // The layer lasts one frame: resubmitted it sounds, and the pulse ends under it.
    engine.SubmitLayer(application, Pad0, RumbleChannels{.LowFrequency = 0.2f});
    scopes.Resolve();
    engine.Update({.Delta = 0.3f});
    CHECK(engine.GetOneShots().empty());
    CHECK(motors.Written[0].LowFrequency == doctest::Approx(0.2f));

    scopes.Resolve();
    engine.Update({.Delta = 0.1f});
    CHECK(motors.Written[0] == RumbleChannels{});
}

TEST_CASE("haptics: a held scope's one-shot is silent and frozen, and resumes where it held")
{
    PresentationScopes scopes;
    Motors motors;
    HapticsEngine engine(scopes, motors.Sink());
    const Unique<PresentationScope> scope = scopes.Open();
    engine.PlayOneShot(scope->GetId(), Pad0, Adopt(Constant(0.5f, 100.0f)));

    struct Frame
    {
        bool Renewed;
        f32 Delta;
    };
    // Held from the play, renewed, held again, then renewed.
    constexpr Frame Frames[] = {
        {.Renewed = false, .Delta = 0.1f}, {.Renewed = false, .Delta = 0.2f},
        {.Renewed = true, .Delta = 0.05f}, {.Renewed = true, .Delta = 0.1f},
        {.Renewed = false, .Delta = 0.3f}, {.Renewed = false, .Delta = 0.25f},
        {.Renewed = true, .Delta = 0.2f},  {.Renewed = true, .Delta = 0.15f}};

    // The first frame that advances sounds the clip's start; every later one adds its delta.
    f32 advanced = 0.0f;
    bool sounded = false;
    usize silentHeld = 0;
    usize heldFrames = 0;
    usize frozenHeld = 0;
    for (const Frame& frame : Frames)
    {
        const f32 before = Only(engine).Time;
        if (frame.Renewed)
        {
            scope->Renew(true);
        }
        scopes.Resolve();
        engine.Update({.Delta = frame.Delta});
        if (frame.Renewed)
        {
            advanced += sounded ? frame.Delta : 0.0f;
            sounded = true;
            continue;
        }
        ++heldFrames;
        silentHeld += motors.Written[0] == RumbleChannels{} ? 1 : 0;
        frozenHeld += Only(engine).Time == before ? 1 : 0;
    }
    CHECK(silentHeld == heldFrames);
    CHECK(frozenHeld == heldFrames);
    CHECK(Only(engine).Time == doctest::Approx(advanced));
    CHECK(motors.Written[0].LowFrequency == doctest::Approx(0.5f));
}

TEST_CASE("haptics: a muted scope's one-shot keeps time silently and ends at its duration")
{
    PresentationScopes scopes;
    Motors motors;
    HapticsEngine engine(scopes, motors.Sink());
    const Unique<PresentationScope> scope = scopes.Open();
    constexpr f32 Duration = 0.5f;
    constexpr f32 Step = 0.125f;
    engine.PlayOneShot(scope->GetId(), Pad0, Adopt(Constant(1.0f, Duration)));

    // The fresh frame sounds the start, then each frame advances; it ends on the frame time reaches
    // its duration, exactly as it would have audibly.
    usize frames = 0;
    bool silent = true;
    while (!engine.GetOneShots().empty() && frames < 16)
    {
        scope->Renew(false);
        scopes.Resolve();
        engine.Update({.Delta = Step});
        ++frames;
        silent = silent && motors.Written[0] == RumbleChannels{};
    }
    CHECK(silent);
    CHECK(frames == 1 + static_cast<usize>(Duration / Step));
}

TEST_CASE("haptics: closing a scope drops its one-shots and layers, and leaves another scope's")
{
    PresentationScopes scopes;
    Motors motors;
    HapticsEngine engine(scopes, motors.Sink());
    Unique<PresentationScope> closing = scopes.Open();
    const Unique<PresentationScope> staying = scopes.Open();
    engine.PlayOneShot(closing->GetId(), Pad0, Adopt(Constant(0.8f, 10.0f)));
    engine.PlayOneShot(staying->GetId(), Pad0, Adopt(Constant(0.3f, 10.0f)));

    const auto frame = [&]
    {
        if (closing)
        {
            closing->Renew(true);
        }
        staying->Renew(true);
        scopes.Resolve();
        engine.Update({.Delta = 0.1f});
    };
    engine.SubmitLayer(closing->GetId(), Pad1, RumbleChannels{.HighFrequency = 0.6f});
    frame();
    CHECK(motors.Written[0].LowFrequency == doctest::Approx(0.8f));
    CHECK(motors.Written[1].HighFrequency == doctest::Approx(0.6f));

    // A layer submitted before the scope closes in the same frame does not sound either.
    engine.SubmitLayer(closing->GetId(), Pad1, RumbleChannels{.HighFrequency = 0.6f});
    const PresentationScopeId closed = closing->GetId();
    closing.reset();
    frame();
    CHECK(motors.Written[0].LowFrequency == doctest::Approx(0.3f));
    CHECK(motors.Written[1] == RumbleChannels{});
    CHECK(Only(engine).Scope == staying->GetId());
    CHECK_FALSE(Only(engine).Scope == closed);

    // A play into a closed scope starts nothing.
    engine.PlayOneShot(closed, Pad0, Adopt(Constant(1.0f, 1.0f)));
    CHECK(engine.GetOneShots().size() == 1);
}

TEST_CASE("haptics: the facade starts nothing inside a replay, nor for a clip still loading")
{
    const PresentationScopes scopes;
    HapticsEngine engine(scopes);
    const Input input{nullptr};
    const AssetHandle<RumbleClip> clip = Adopt(Constant(1.0f, 1.0f));
    const RumbleTarget target = RumbleTarget::ForGamepad(Pad0);

    const ScopedHaptics replaying(engine, scopes.GetApplicationScope(), input, nullptr, true);
    replaying.PlayOneShot(target, clip);
    CHECK(engine.GetOneShots().empty());

    const ScopedHaptics live(engine, scopes.GetApplicationScope(), input, nullptr, false);
    live.PlayOneShot(target, AssetHandle<RumbleClip>{});
    CHECK(engine.GetOneShots().empty());
    live.PlayOneShot(target, clip);
    CHECK(engine.GetOneShots().size() == 1);
}

TEST_CASE("haptics: a seat target resolves in its own scene, whose seats share handles with others")
{
    // The registry first: each scene's scope closes into it when the scene goes.
    PresentationScopes scopes;
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    const Unique<Scene> first = Scene::Create(types);
    const Unique<Scene> second = Scene::Create(types);
    const Entity firstSeat = first->CreateEntity();
    const Entity secondSeat = second->CreateEntity();
    REQUIRE(firstSeat == secondSeat);
    first->Add<SeatInput>(firstSeat, SeatInput{.Gamepad = Pad1});
    second->Add<SeatInput>(secondSeat, SeatInput{.Gamepad = Pad3});

    Motors motors;
    HapticsEngine engine(scopes, motors.Sink());
    const Input input{nullptr};
    first->SetPresentationScope(scopes.Open());
    second->SetPresentationScope(scopes.Open());
    const AssetHandle<RumbleClip> clip = Adopt(Constant(0.6f, 5.0f));
    for (const Scene* scene : {first.get(), second.get()})
    {
        const ScopedHaptics haptics(engine, scene->GetPresentationScope()->GetId(), input, scene,
                                    false);
        haptics.PlayOneShot(RumbleTarget::ForSeat(firstSeat), clip);
        // The implicit seat reads every device; headless, none is connected.
        haptics.PlayOneShot(RumbleTarget::ForSeat(Entity::Null), clip);
        scene->GetPresentationScope()->Renew(true);
    }
    scopes.Resolve();
    engine.Update({.Delta = 0.0f});

    CHECK(engine.GetOneShots().size() == 2);
    CHECK(motors.Written[1].LowFrequency == doctest::Approx(0.6f));
    CHECK(motors.Written[3].LowFrequency == doctest::Approx(0.6f));
    CHECK(motors.Written[0] == RumbleChannels{});
    CHECK(motors.Written[2] == RumbleChannels{});
}

TEST_CASE("haptics: a suspended output writes zero but keeps the mix and the clock")
{
    PresentationScopes scopes;
    Motors motors;
    HapticsEngine engine(scopes, motors.Sink());
    engine.SetMasterIntensity(0.5f);
    engine.PlayOneShot(scopes.GetApplicationScope(), Pad0, Adopt(Constant(1.0f, 1.0f)));
    scopes.Resolve();
    engine.Update({.Delta = 0.0f, .OutputSuspended = true});
    CHECK(engine.IsOutputSuspended());
    CHECK(motors.Written[0] == RumbleChannels{});
    CHECK(engine.GetOutput(Pad0).LowFrequency == doctest::Approx(0.5f));

    // The pulse ends while suspended, so it does not replay when the output returns.
    engine.Update({.Delta = 2.0f, .OutputSuspended = true});
    CHECK(engine.GetOneShots().empty());
    engine.Update({.Delta = 0.0f});
    CHECK(motors.Written[0] == RumbleChannels{});
}

TEST_CASE("haptics: HapticsSystem plays a RumbleSource with its world's pause, entity and fade")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;
    systems.Register<HapticsSystem>();
    TestSupport::TestServices services;
    PresentationScopes& scopes = services.GetPresentationScopes();
    HapticsEngine& engine = services.GetHaptics();
    WorldRunner runner(
        WorldRunnerInfo{.Types = &types, .Systems = &systems, .Presentation = &scopes});
    // A presented world, so its scope is Live; the factory is otherwise the bundle's own.
    runner.SetContextFactory(
        [&services](const SystemContextRequest& request)
        {
            SystemContext context = services.Make(request);
            context.View = SystemViewInfo{};
            return context;
        });
    const WorldInstanceId world =
        runner.OpenWorld(WorldOpenInfo{.Systems = vector<SystemId>{SystemIdOf<HapticsSystem>()}});
    Scene& scene = runner.ResolveWorld(world)->GetScene();

    constexpr f32 Step = 0.125f;
    const auto frame = [&]
    {
        runner.Tick(WorldTickInfo{.Delta = Step});
        scopes.Resolve();
        engine.Update({.Delta = Step});
    };
    const auto level = [&] { return engine.GetOutput(Pad1).LowFrequency; };

    const Entity seat = scene.CreateEntity();
    scene.Add<SeatInput>(seat, SeatInput{.Gamepad = Pad1});
    const Entity emitter = scene.CreateEntity();

    SUBCASE("a once-through source clears Playing after its duration")
    {
        constexpr f32 Duration = 0.25f;
        scene.Add<RumbleSource>(emitter, RumbleSource{.Clip = Adopt(Constant(0.6f, Duration)),
                                                      .Seat = seat,
                                                      .Loop = RumbleLoop::Once});
        usize sounding = 0;
        for (usize i = 0; i < 6; ++i)
        {
            frame();
            sounding += level() > 0.0f ? 1 : 0;
        }
        CHECK(sounding == static_cast<usize>(Duration / Step));
        CHECK_FALSE(scene.Get<RumbleSource>(emitter).Playing);
        CHECK(level() == 0.0f);
    }

    SUBCASE("a paused world holds its source's time and silences the pad, then resumes from it")
    {
        scene.Add<RumbleSource>(
            emitter, RumbleSource{.Clip = Adopt(Constant(0.6f, 1.0f, true)), .Seat = seat});
        frame();
        frame();
        const f32 held = scene.Get<RumbleSource>(emitter).Time;
        CHECK(level() == doctest::Approx(0.6f));

        runner.SetWorldPaused(world, true);
        frame();
        frame();
        CHECK(scene.Get<RumbleSource>(emitter).Time == held);
        CHECK(level() == 0.0f);

        runner.SetWorldPaused(world, false);
        frame();
        CHECK(scene.Get<RumbleSource>(emitter).Time == doctest::Approx(held + Step));
        CHECK(level() == doctest::Approx(0.6f));
    }

    SUBCASE("destroying the source's entity silences the pad the next frame")
    {
        scene.Add<RumbleSource>(
            emitter, RumbleSource{.Clip = Adopt(Constant(0.6f, 1.0f, true)), .Seat = seat});
        frame();
        CHECK(level() == doctest::Approx(0.6f));
        scene.DestroyEntity(emitter);
        frame();
        CHECK(level() == 0.0f);
    }

    SUBCASE("a source with a fade ramps to silence monotonically once Playing goes false")
    {
        constexpr f32 Fade = 0.5f;
        scene.Add<RumbleSource>(emitter, RumbleSource{.Clip = Adopt(Constant(1.0f, 1.0f, true)),
                                                      .Seat = seat,
                                                      .FadeOutSeconds = Fade});
        frame();
        CHECK(level() == doctest::Approx(1.0f));

        scene.Get<RumbleSource>(emitter).Playing = false;
        f32 previous = level();
        bool monotone = true;
        usize fading = 0;
        for (usize i = 0; i < 8; ++i)
        {
            frame();
            monotone = monotone && level() <= previous;
            fading += level() > 0.0f ? 1 : 0;
            previous = level();
        }
        CHECK(monotone);
        CHECK(fading > 0);
        CHECK(fading < static_cast<usize>(Fade / Step) + 1);
        CHECK(level() == 0.0f);
    }
}
