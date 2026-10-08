// The View-phase AudioSystem over a Scene and a null device: it publishes a well-formed voice
// snapshot (a PlayOnStart source sounds, a no-listener scene still plays non-spatial voices, the cap
// keeps the loudest, a finished non-looping source is gone next tick), and it reads the interpolated
// drawn pose rather than the raw Sim transform. Pure CPU — the null device runs the whole mix path
// on the main thread, so no hardware is touched.

#include <doctest/doctest.h>

#include <glm/gtc/epsilon.hpp>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Audio/AudioClip.h>
#include <Veng/Audio/AudioComponents.h>
#include <Veng/Audio/AudioDevice.h>
#include <Veng/Audio/AudioEngine.h>
#include <Veng/Audio/AudioSystem.h>
#include <Veng/Audio/ScopedAudio.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/PresentationScope.h>
#include <Veng/Scene/SceneSystem.h>
#include <Veng/Scene/SystemRegistry.h>
#include <Veng/World.h>
#include <Veng/WorldRunner.h>
#include "support/TestServices.h"

#include <cmath>
#include <cstring>
#include <numbers>
#include <vector>

using namespace Veng;
using namespace Veng::Audio;

namespace
{
    // A resident Pcm clip wrapped in an AssetHandle via Adopt — the cheapest loadable clip a source
    // can name, built without a cook by hand-assembling the cooked blob a Pcm decode expects.
    AssetHandle<Audio::AudioClip> MakePcmClip(const f32 value, const u32 frames)
    {
        const CookedAudioHeader header{
            .Version = CookedAudioVersion,
            .Storage = static_cast<u32>(CookedAudioStorage::Pcm),
            .SampleFormat = static_cast<u32>(CookedAudioSampleFormat::F32),
            .Codec = static_cast<u32>(CookedAudioCodec::None),
            .SampleRate = 48000,
            .Channels = 1,
            .FrameCount = frames,
        };
        std::vector<u8> blob(sizeof(header) + static_cast<usize>(frames) * sizeof(f32));
        std::memcpy(blob.data(), &header, sizeof(header));
        const std::vector<f32> samples(frames, value);
        std::memcpy(blob.data() + sizeof(header), samples.data(),
                    static_cast<usize>(frames) * sizeof(f32));

        const Result<Ref<Audio::AudioClip>> clip = Audio::AudioClip::Decode(blob);
        REQUIRE(clip.has_value());
        return AssetManager::Adopt<Audio::AudioClip>(*clip);
    }
}

TEST_CASE("a PlayOnStart non-spatial source plays with no listener in the scene")
{
    TypeRegistry registry;
    RegisterBuiltinTypes(registry);
    const Unique<Scene> scene = Scene::Create(registry);
    TestSupport::TestServices services;
    const AudioDevice& device = services.GetAudioDevice();

    const Entity entity = scene->CreateEntity();
    scene->Add<Transform>(entity, Transform{});
    scene->Add<AudioSource>(entity, AudioSource{.Clip = MakePcmClip(0.5f, 4800),
                                                .Bus = "Music",
                                                .PlayOnStart = true,
                                                .Spatial = false});

    AudioSystem system;
    system.OnStart(*scene, services.Make());
    system.OnUpdate(*scene, 1.0f / 60.0f, services.Make());

    // No AudioListener anywhere, yet the non-spatial voice plays — the listener-at-origin fallback.
    CHECK(device.GetEngine().GetActiveVoiceCount() == 1);
    CHECK(system.HasVoice(entity));
}

TEST_CASE("a finished non-looping source is gone the next tick")
{
    TypeRegistry registry;
    RegisterBuiltinTypes(registry);
    const Unique<Scene> scene = Scene::Create(registry);
    TestSupport::TestServices services;
    AudioDevice& device = services.GetAudioDevice();

    const Entity entity = scene->CreateEntity();
    scene->Add<Transform>(entity, Transform{});
    // A single-frame one-shot: it exhausts within one pump.
    scene->Add<AudioSource>(entity, AudioSource{.Clip = MakePcmClip(0.5f, 1),
                                                .Looping = false,
                                                .PlayOnStart = true,
                                                .Spatial = false});

    AudioSystem system;
    system.OnStart(*scene, services.Make());
    system.OnUpdate(*scene, 1.0f / 60.0f, services.Make());
    CHECK(system.HasVoice(entity));

    // Pump plays the one-shot out and drains the retired-voice channel.
    for (int i = 0; i < 4 && device.GetEngine().GetActiveVoiceCount() > 0; ++i)
    {
        device.Pump(1.0f / 60.0f);
    }

    // The system learns of the retirement through IsVoiceLive and drops the voice, and does not
    // restart the finished one-shot on any later tick.
    system.OnUpdate(*scene, 1.0f / 60.0f, services.Make());
    CHECK_FALSE(system.HasVoice(entity));
    CHECK(device.GetEngine().GetActiveVoiceCount() == 0);
    system.OnUpdate(*scene, 1.0f / 60.0f, services.Make());
    CHECK(device.GetEngine().GetActiveVoiceCount() == 0);
}

TEST_CASE("a looping source persists across pumps")
{
    TypeRegistry registry;
    RegisterBuiltinTypes(registry);
    const Unique<Scene> scene = Scene::Create(registry);
    TestSupport::TestServices services;
    AudioDevice& device = services.GetAudioDevice();

    const Entity entity = scene->CreateEntity();
    scene->Add<Transform>(entity, Transform{});
    scene->Add<AudioSource>(entity, AudioSource{.Clip = MakePcmClip(0.5f, 64),
                                                .Looping = true,
                                                .PlayOnStart = true,
                                                .Spatial = false});

    AudioSystem system;
    system.OnStart(*scene, services.Make());
    system.OnUpdate(*scene, 1.0f / 60.0f, services.Make());
    for (int i = 0; i < 10; ++i)
    {
        device.Pump(1.0f / 60.0f);
        system.OnUpdate(*scene, 1.0f / 60.0f, services.Make());
    }
    CHECK(system.HasVoice(entity));
    CHECK(device.GetEngine().GetActiveVoiceCount() == 1);
}

TEST_CASE("the voice cap keeps the loudest sources")
{
    TypeRegistry registry;
    RegisterBuiltinTypes(registry);
    const Unique<Scene> scene = Scene::Create(registry);
    TestSupport::TestServices services;
    const AudioDevice& device = services.GetAudioDevice();

    const AssetHandle<Audio::AudioClip> clip = MakePcmClip(0.5f, 64);
    const Entity listener = scene->CreateEntity();
    scene->Add<Transform>(listener, Transform{});
    scene->Add<AudioListener>(listener, AudioListener{.Gain = 1.0f});

    // Four non-spatial sources of distinct loudness; the cap of two keeps the two loudest.
    std::vector<Entity> sources;
    for (const f32 gain : {0.1f, 0.2f, 0.3f, 0.4f})
    {
        const Entity entity = scene->CreateEntity();
        scene->Add<Transform>(entity, Transform{});
        scene->Add<AudioSource>(
            entity, AudioSource{.Clip = clip, .Gain = gain, .PlayOnStart = true, .Spatial = false});
        sources.push_back(entity);
    }

    AudioSystem system;
    system.SetVoiceCap(2);
    system.OnStart(*scene, services.Make());
    system.OnUpdate(*scene, 1.0f / 60.0f, services.Make());

    CHECK(device.GetEngine().GetActiveVoiceCount() == 2);
    CHECK_FALSE(system.HasVoice(sources[0])); // gain 0.1 — dropped
    CHECK_FALSE(system.HasVoice(sources[1])); // gain 0.2 — dropped
    CHECK(system.HasVoice(sources[2]));       // gain 0.3 — kept
    CHECK(system.HasVoice(sources[3]));       // gain 0.4 — kept
}

TEST_CASE("the system places a source at its interpolated drawn pose, not the raw Sim transform")
{
    TypeRegistry registry;
    RegisterBuiltinTypes(registry);
    const Unique<Scene> scene = Scene::Create(registry);
    TestSupport::TestServices services;
    const AudioDevice& device = services.GetAudioDevice();

    const Entity entity = scene->CreateEntity();
    scene->Add<Transform>(entity, Transform{.Position = {0.0f, 0.0f, 0.0f}});
    scene->Add<AudioSource>(
        entity, AudioSource{.Clip = MakePcmClip(0.5f, 4800), .PlayOnStart = true, .Spatial = true});

    // Two ticks of motion so the history ring holds {x=0, x=10}; the raw transform is x=10.
    scene->SnapshotTransformHistory();
    scene->Get<Transform>(entity).Position = {10.0f, 0.0f, 0.0f};
    scene->SnapshotTransformHistory();
    REQUIRE(scene->HasTransformInterpolation());

    AudioSystem system;
    system.OnStart(*scene, services.Make());
    system.OnUpdate(*scene, 1.0f / 60.0f, services.Make().WithAlpha(0.5f));

    // At alpha 0.5 the drawn pose is x=5, the midpoint the renderer blends to — not the Sim tick's
    // x=10 the un-interpolated transform holds.
    const optional<vec3> position = system.GetDebugSourcePosition(entity);
    REQUIRE(position.has_value());
    CHECK(glm::all(glm::epsilonEqual(*position, vec3(5.0f, 0.0f, 0.0f), 1e-4f)));
}

namespace
{
    // A scene holding a scope of the bundle's registry, with a listener at `listenerX` facing -Z
    // (right is +X), the shape every scene a runner holds has.
    struct ListeningScene
    {
        Unique<Veng::Scene> Scene;
        AudioSystem System;

        ListeningScene(TypeRegistry& types, TestSupport::TestServices& services,
                       const f32 listenerX)
            : Scene(Veng::Scene::Create(types))
        {
            Scene->SetPresentationScope(services.GetPresentationScopes().Open());
            const Entity listener = Scene->CreateEntity();
            Scene->Add<Transform>(listener, Transform{.Position = vec3(listenerX, 0.0f, 0.0f)});
            Scene->Add<AudioListener>(listener, AudioListener{});
        }

        SystemContext Context(TestSupport::TestServices& services) const
        {
            return services.Make(SystemContextRequest{
                .World = WorldInstanceId{1}, .Scene = *Scene, .Phase = SystemContextPhase::View});
        }
    };
}

TEST_CASE(
    "a PlayAt voice pans toward the listener of its own scene, whatever order scenes update in")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    TestSupport::TestServices services;
    AudioEngine& engine = services.GetAudio();
    ListeningScene left(types, services, -10.0f);
    ListeningScene right(types, services, 10.0f);
    const AssetHandle<Audio::AudioClip> clip = MakePcmClip(0.5f, 48000);
    constexpr f32 Delta = 1.0f / 60.0f;

    // Each scene fires one voice at the origin: to the left scene's listener it is on the right.
    left.System.OnUpdate(*left.Scene, Delta, left.Context(services));
    right.System.OnUpdate(*right.Scene, Delta, right.Context(services));
    const SpatialOneShotParams params{.Loop = true, .MaxDistance = 100.0f};
    const VoiceHandle fromLeft = left.Context(services).Audio.PlayAt(clip, vec3(0.0f), params);
    const VoiceHandle fromRight = right.Context(services).Audio.PlayAt(clip, vec3(0.0f), params);
    REQUIRE(fromLeft.IsValid());
    REQUIRE(fromRight.IsValid());

    for (const bool leftFirst : {true, false})
    {
        ListeningScene& first = leftFirst ? left : right;
        ListeningScene& second = leftFirst ? right : left;
        first.System.OnUpdate(*first.Scene, Delta, first.Context(services));
        second.System.OnUpdate(*second.Scene, Delta, second.Context(services));
        engine.Update(Delta);
        CHECK(engine.GetVoiceParams(fromLeft)->Pan > 0.5f);
        CHECK(engine.GetVoiceParams(fromRight)->Pan < -0.5f);
    }
}

TEST_CASE("the music crossfade advances once a frame however many scenes run an AudioSystem")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    TestSupport::TestServices services;
    AudioEngine& engine = services.GetAudio();
    vector<Unique<ListeningScene>> scenes;
    for (int i = 0; i < 3; ++i)
    {
        scenes.push_back(CreateUnique<ListeningScene>(types, services, 0.0f));
    }

    constexpr f32 Fade = 1.0f;
    constexpr f32 Delta = 1.0f / 60.0f;
    constexpr int Frames = 30;
    engine.Music().Set(MakePcmClip(0.5f, 48000), MusicTransition{.FadeSeconds = Fade});
    for (int frame = 0; frame < Frames; ++frame)
    {
        for (const Unique<ListeningScene>& scene : scenes)
        {
            scene->System.OnUpdate(*scene->Scene, Delta, scene->Context(services));
        }
        engine.Update(Delta);
    }

    // The incoming track's equal-power gain at fade progress T / FadeSeconds.
    const vector<MusicDirector::VoiceState> states = engine.Music().GetVoiceStates();
    REQUIRE(states.size() == 1);
    const f32 progress = static_cast<f32>(Frames) * Delta / Fade;
    CHECK(states.front().Gain ==
          doctest::Approx(std::sin(progress * std::numbers::pi_v<f32> * 0.5f)).epsilon(1e-3));
}

TEST_CASE("a paused runner world's source voice is held, and plays again on resume")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;
    systems.Register<AudioSystem>();
    TestSupport::TestServices services;
    PresentationScopes& scopes = services.GetPresentationScopes();
    AudioEngine& engine = services.GetAudio();
    WorldRunner runner(
        WorldRunnerInfo{.Types = &types, .Systems = &systems, .Presentation = &scopes});
    // A presented world, so its scope is Live while it runs; the factory is otherwise the bundle's.
    runner.SetContextFactory(
        [&services](const SystemContextRequest& request)
        {
            SystemContext context = services.Make(request);
            context.View = SystemViewInfo{};
            return context;
        });
    const WorldInstanceId world =
        runner.OpenWorld(WorldOpenInfo{.Systems = vector<SystemId>{SystemIdOf<AudioSystem>()}});
    Scene& scene = runner.ResolveWorld(world)->GetScene();
    const Entity emitter = scene.CreateEntity();
    scene.Add<Transform>(emitter, Transform{});
    scene.Add<AudioSource>(emitter, AudioSource{.Clip = MakePcmClip(0.5f, 4800),
                                                .Looping = true,
                                                .PlayOnStart = true,
                                                .Spatial = false});

    const auto frame = [&]
    {
        runner.Tick(WorldTickInfo{.Delta = 1.0f / 60.0f});
        scopes.Resolve();
        engine.Update(1.0f / 60.0f);
    };
    const auto state = [&]
    {
        const vector<VoiceInfo> voices = engine.GetVoiceInfos();
        REQUIRE(voices.size() == 1);
        return voices.front().State;
    };

    frame();
    CHECK(state() == PresentationState::Live);

    runner.SetWorldPaused(world, true);
    frame();
    CHECK(state() == PresentationState::Held);

    runner.SetWorldPaused(world, false);
    frame();
    CHECK(state() == PresentationState::Live);
}
