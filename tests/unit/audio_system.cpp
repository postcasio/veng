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
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneSystem.h>

#include <cstring>
#include <vector>

using namespace Veng;
using namespace Veng::Audio;

namespace
{
    // A SystemContext the AudioSystem never dereferences: it reads only Alpha, never the Assets,
    // Input, or Tasks services, so the backing storage is never touched.
    struct ContextStorage
    {
        alignas(16) unsigned char InputBytes[64]{};
        alignas(16) unsigned char TasksBytes[64]{};
        alignas(16) unsigned char AssetsBytes[64]{};

        SystemContext Make()
        {
            return SystemContext{
                .Assets = *reinterpret_cast<AssetManager*>(AssetsBytes),
                .Input = *reinterpret_cast<Input*>(InputBytes),
                .Tasks = *reinterpret_cast<TaskSystem*>(TasksBytes),
            };
        }
    };

    Unique<AudioDevice> MakeNullDevice()
    {
        return AudioDevice::Create(
            AudioDeviceInfo{.Backend = AudioBackend::Null, .SampleRate = 48000, .Channels = 2});
    }

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
    const Unique<AudioDevice> device = MakeNullDevice();

    const Entity entity = scene->CreateEntity();
    scene->Add<Transform>(entity, Transform{});
    scene->Add<AudioSource>(entity, AudioSource{.Clip = MakePcmClip(0.5f, 4800),
                                                .Bus = Audio::AudioBus::Music,
                                                .PlayOnStart = true,
                                                .Spatial = false});

    AudioSystem system;
    system.SetEngine(&device->GetEngine());
    ContextStorage storage;
    system.OnStart(*scene, storage.Make());
    system.OnUpdate(*scene, 1.0f / 60.0f, storage.Make());

    // No AudioListener anywhere, yet the non-spatial voice plays — the listener-at-origin fallback.
    CHECK(device->GetEngine().GetActiveVoiceCount() == 1);
    CHECK(system.HasVoice(entity));
}

TEST_CASE("a finished non-looping source is gone the next tick")
{
    TypeRegistry registry;
    RegisterBuiltinTypes(registry);
    const Unique<Scene> scene = Scene::Create(registry);
    const Unique<AudioDevice> device = MakeNullDevice();

    const Entity entity = scene->CreateEntity();
    scene->Add<Transform>(entity, Transform{});
    // A single-frame one-shot: it exhausts within one pump.
    scene->Add<AudioSource>(entity, AudioSource{.Clip = MakePcmClip(0.5f, 1),
                                                .Looping = false,
                                                .PlayOnStart = true,
                                                .Spatial = false});

    AudioSystem system;
    system.SetEngine(&device->GetEngine());
    ContextStorage storage;
    system.OnStart(*scene, storage.Make());
    system.OnUpdate(*scene, 1.0f / 60.0f, storage.Make());
    CHECK(system.HasVoice(entity));

    // Pump plays the one-shot out and drains the retired-voice channel.
    for (int i = 0; i < 4 && device->GetEngine().GetActiveVoiceCount() > 0; ++i)
    {
        device->Pump(1.0f / 60.0f);
    }

    // The system learns of the retirement through IsVoiceLive and drops the voice, and does not
    // restart the finished one-shot on any later tick.
    system.OnUpdate(*scene, 1.0f / 60.0f, storage.Make());
    CHECK_FALSE(system.HasVoice(entity));
    CHECK(device->GetEngine().GetActiveVoiceCount() == 0);
    system.OnUpdate(*scene, 1.0f / 60.0f, storage.Make());
    CHECK(device->GetEngine().GetActiveVoiceCount() == 0);
}

TEST_CASE("a looping source persists across pumps")
{
    TypeRegistry registry;
    RegisterBuiltinTypes(registry);
    const Unique<Scene> scene = Scene::Create(registry);
    const Unique<AudioDevice> device = MakeNullDevice();

    const Entity entity = scene->CreateEntity();
    scene->Add<Transform>(entity, Transform{});
    scene->Add<AudioSource>(entity, AudioSource{.Clip = MakePcmClip(0.5f, 64),
                                                .Looping = true,
                                                .PlayOnStart = true,
                                                .Spatial = false});

    AudioSystem system;
    system.SetEngine(&device->GetEngine());
    ContextStorage storage;
    system.OnStart(*scene, storage.Make());
    system.OnUpdate(*scene, 1.0f / 60.0f, storage.Make());
    for (int i = 0; i < 10; ++i)
    {
        device->Pump(1.0f / 60.0f);
        system.OnUpdate(*scene, 1.0f / 60.0f, storage.Make());
    }
    CHECK(system.HasVoice(entity));
    CHECK(device->GetEngine().GetActiveVoiceCount() == 1);
}

TEST_CASE("the voice cap keeps the loudest sources")
{
    TypeRegistry registry;
    RegisterBuiltinTypes(registry);
    const Unique<Scene> scene = Scene::Create(registry);
    const Unique<AudioDevice> device = MakeNullDevice();

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
    system.SetEngine(&device->GetEngine());
    system.SetVoiceCap(2);
    ContextStorage storage;
    system.OnStart(*scene, storage.Make());
    system.OnUpdate(*scene, 1.0f / 60.0f, storage.Make());

    CHECK(device->GetEngine().GetActiveVoiceCount() == 2);
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
    const Unique<AudioDevice> device = MakeNullDevice();

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
    system.SetEngine(&device->GetEngine());
    ContextStorage storage;
    system.OnStart(*scene, storage.Make());
    system.OnUpdate(*scene, 1.0f / 60.0f, storage.Make().WithAlpha(0.5f));

    // At alpha 0.5 the drawn pose is x=5, the midpoint the renderer blends to — not the Sim tick's
    // x=10 the un-interpolated transform holds.
    const optional<vec3> position = system.GetDebugSourcePosition(entity);
    REQUIRE(position.has_value());
    CHECK(glm::all(glm::epsilonEqual(*position, vec3(5.0f, 0.0f, 0.0f), 1e-4f)));
}
