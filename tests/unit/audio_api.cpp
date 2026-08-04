// The code-facing audio surface over a null device — all pure CPU, no hardware. A one-shot lands in
// the snapshot then retires on its own or on demand; a PlayAt voice shares the spatialization path;
// the one-shot pool caps and drops the quietest; and the music director holds one logical track,
// crossfading equal-power between two and collapsing to one, with an authored MusicState starting a
// level's track once on world start.

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Audio/AudioClip.h>
#include <Veng/Audio/AudioComponents.h>
#include <Veng/Audio/AudioDevice.h>
#include <Veng/Audio/AudioEngine.h>
#include <Veng/Audio/AudioSystem.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneSystem.h>

#include <algorithm>
#include <cstring>
#include <vector>

using namespace Veng;
using namespace Veng::Audio;

namespace
{
    Unique<AudioDevice> MakeNullDevice()
    {
        return AudioDevice::Create(
            AudioDeviceInfo{.Backend = AudioBackend::Null, .SampleRate = 48000, .Channels = 2});
    }

    // A resident Pcm clip wrapped in an AssetHandle via Adopt — a loadable clip built without a cook
    // by hand-assembling the cooked blob a Pcm decode expects.
    AssetHandle<AudioClip> MakePcmClip(const f32 value, const u32 frames)
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

        const Result<Ref<AudioClip>> clip = AudioClip::Decode(blob);
        REQUIRE(clip.has_value());
        return AssetManager::Adopt<AudioClip>(*clip);
    }

    // A SystemContext the AudioSystem never dereferences beyond context.Audio: Assets, Input, and
    // Tasks stay faked while the audio engine is the live one under test.
    struct ContextStorage
    {
        alignas(16) unsigned char InputBytes[64]{};
        alignas(16) unsigned char TasksBytes[64]{};
        alignas(16) unsigned char AssetsBytes[64]{};

        SystemContext Make(AudioEngine& engine)
        {
            return SystemContext{
                .Assets = *reinterpret_cast<AssetManager*>(AssetsBytes),
                .Input = *reinterpret_cast<Input*>(InputBytes),
                .Tasks = *reinterpret_cast<TaskSystem*>(TasksBytes),
                .Audio = engine,
            };
        }
    };

    // The applied gain of the pair member that is (or is not) fading out; -1 when absent.
    f32 GainOf(const vector<MusicDirector::VoiceState>& states, const bool fadingOut)
    {
        for (const MusicDirector::VoiceState& state : states)
        {
            if (state.FadingOut == fadingOut)
            {
                return state.Gain;
            }
        }
        return -1.0f;
    }
}

TEST_CASE("a one-shot appears in the snapshot, retires after its duration, and stops on demand")
{
    const Unique<AudioDevice> device = MakeNullDevice();
    AudioEngine& engine = device->GetEngine();

    // A short one-shot: one pump (800 frames at 48 kHz / 60 Hz) plays its 8 frames out.
    const VoiceHandle voice =
        engine.PlayOneShot(MakePcmClip(0.5f, 8), OneShotParams{.Bus = AudioBus::SFX, .Gain = 0.8f});
    REQUIRE(voice.IsValid());
    CHECK(engine.GetActiveVoiceCount() == 1);
    CHECK(engine.IsVoiceLive(voice));

    for (int i = 0; i < 6 && engine.GetActiveVoiceCount() > 0; ++i)
    {
        device->Pump(1.0f / 60.0f);
    }
    CHECK_FALSE(engine.IsVoiceLive(voice));
    CHECK(engine.GetActiveVoiceCount() == 0);

    // The returned handle stops a still-playing voice early.
    const VoiceHandle held =
        engine.PlayOneShot(MakePcmClip(0.5f, 48000), OneShotParams{.Loop = true});
    REQUIRE(held.IsValid());
    CHECK(engine.GetActiveVoiceCount() == 1);
    engine.StopVoice(held);
    CHECK_FALSE(engine.IsVoiceLive(held));
    CHECK(engine.GetActiveVoiceCount() == 0);
}

TEST_CASE("PlayAt places a spatial voice that pans toward the source")
{
    const Unique<AudioDevice> device = MakeNullDevice();
    AudioEngine& engine = device->GetEngine();
    const AssetHandle<AudioClip> clip = MakePcmClip(0.5f, 48000);

    // Listener at the origin, identity rotation (its +X is right). A source hard-left (-X) pans left.
    const VoiceHandle left =
        engine.PlayAt(clip, vec3(-10.0f, 0.0f, 0.0f), SpatialOneShotParams{.MaxDistance = 100.0f});
    REQUIRE(left.IsValid());
    const optional<VoiceParams> leftParams = engine.GetVoiceParams(left);
    REQUIRE(leftParams.has_value());
    CHECK(leftParams->Pan < -0.5f);

    const VoiceHandle right =
        engine.PlayAt(clip, vec3(10.0f, 0.0f, 0.0f), SpatialOneShotParams{.MaxDistance = 100.0f});
    REQUIRE(right.IsValid());
    CHECK(engine.GetVoiceParams(right)->Pan > 0.5f);

    // SetVoicePose moves the voice to the other side; the pan follows.
    engine.SetVoicePose(left, vec3(10.0f, 0.0f, 0.0f), vec3(0.0f));
    CHECK(engine.GetVoiceParams(left)->Pan > 0.5f);
}

TEST_CASE("the one-shot pool caps at MaxOneShotVoices and drops the quietest")
{
    const Unique<AudioDevice> device = MakeNullDevice();
    AudioEngine& engine = device->GetEngine();
    const AssetHandle<AudioClip> clip = MakePcmClip(0.5f, 48000);

    // A burst past the pool in strictly increasing loudness; the pool keeps the loudest.
    const u32 burst = MaxOneShotVoices + 8;
    std::vector<VoiceHandle> handles;
    std::vector<f32> gains;
    for (u32 i = 0; i < burst; ++i)
    {
        const f32 gain = 0.1f + 0.01f * static_cast<f32>(i);
        gains.push_back(gain);
        handles.push_back(engine.PlayOneShot(clip, OneShotParams{.Gain = gain, .Loop = true}));
    }

    // Aggregate: exactly the cap survive, and every survivor is louder than every dropped voice.
    CHECK(engine.GetManagedVoiceCount() == MaxOneShotVoices);
    usize live = 0;
    f32 minLiveGain = 2.0f;
    f32 maxDroppedGain = 0.0f;
    for (u32 i = 0; i < burst; ++i)
    {
        if (engine.IsVoiceLive(handles[i]))
        {
            ++live;
            minLiveGain = std::min(minLiveGain, gains[i]);
        }
        else
        {
            maxDroppedGain = std::max(maxDroppedGain, gains[i]);
        }
    }
    CHECK(live == MaxOneShotVoices);
    CHECK(minLiveGain > maxDroppedGain);
}

TEST_CASE("the music director keeps one logical track and crossfades equal-power")
{
    const Unique<AudioDevice> device = MakeNullDevice();
    AudioEngine& engine = device->GetEngine();
    MusicDirector& music = engine.Music();
    const AssetHandle<AudioClip> trackA = MakePcmClip(0.5f, 48000);
    const AssetHandle<AudioClip> trackB = MakePcmClip(0.4f, 48000);

    // Hard-cut A in: it is the one logical track at full gain.
    music.Set(trackA, MusicTransition{.FadeSeconds = 0.0f, .Loop = true});
    REQUIRE(music.GetVoiceCount() == 1);
    CHECK(music.Current().Get() == trackA.Get());
    const VoiceHandle aVoice = music.GetVoiceStates().front().Voice;

    // Set(A) again is a no-op: no re-trigger, no second voice.
    music.Set(trackA, MusicTransition{.FadeSeconds = 0.5f});
    CHECK(music.GetVoiceCount() == 1);
    CHECK(music.GetVoiceStates().front().Voice == aVoice);

    // Crossfade to B over one second. At the start A is full, B silent.
    music.Set(trackB, MusicTransition{.FadeSeconds = 1.0f, .Loop = true});
    REQUIRE(music.GetVoiceCount() == 2);
    CHECK(music.Current().Get() == trackB.Get());
    const vector<MusicDirector::VoiceState> atStart = music.GetVoiceStates();
    CHECK(GainOf(atStart, true) == doctest::Approx(1.0f));
    CHECK(GainOf(atStart, false) == doctest::Approx(0.0f));

    // Midpoint: equal-power means both sit at cos(pi/4) and the power sum stays unity.
    engine.UpdateManagedVoices(ListenerPose{}, 0.5f);
    const vector<MusicDirector::VoiceState> atMid = music.GetVoiceStates();
    const f32 outMid = GainOf(atMid, true);
    const f32 inMid = GainOf(atMid, false);
    CHECK(outMid == doctest::Approx(0.70710677f).epsilon(0.01f));
    CHECK(inMid == doctest::Approx(0.70710677f).epsilon(0.01f));
    CHECK(outMid * outMid + inMid * inMid == doctest::Approx(1.0f).epsilon(0.01f));

    // After the full fade exactly one voice remains: B, no longer fading.
    engine.UpdateManagedVoices(ListenerPose{}, 0.5f);
    CHECK(music.GetVoiceCount() == 1);
    CHECK(music.Current().Get() == trackB.Get());
    const vector<MusicDirector::VoiceState> atEnd = music.GetVoiceStates();
    REQUIRE(atEnd.size() == 1);
    CHECK_FALSE(atEnd.front().FadingOut);
    CHECK(atEnd.front().Gain == doctest::Approx(1.0f));
}

TEST_CASE("Stop empties the Music bus after its fade and Current reports none")
{
    const Unique<AudioDevice> device = MakeNullDevice();
    AudioEngine& engine = device->GetEngine();
    MusicDirector& music = engine.Music();

    music.Set(MakePcmClip(0.5f, 48000), MusicTransition{.FadeSeconds = 0.0f, .Loop = true});
    REQUIRE(music.GetVoiceCount() == 1);

    music.Stop(0.5f);
    CHECK_FALSE(music.Current().IsValid());

    // The bus is still fading for the half second, then empty.
    engine.UpdateManagedVoices(ListenerPose{}, 0.25f);
    CHECK(music.GetVoiceCount() == 1);
    engine.UpdateManagedVoices(ListenerPose{}, 0.25f);
    CHECK(music.GetVoiceCount() == 0);
}

TEST_CASE("an authored MusicState starts its track once on world start")
{
    TypeRegistry registry;
    RegisterBuiltinTypes(registry);
    const Unique<Scene> scene = Scene::Create(registry);
    const Unique<AudioDevice> device = MakeNullDevice();
    AudioEngine& engine = device->GetEngine();

    const AssetHandle<AudioClip> track = MakePcmClip(0.5f, 48000);
    const Entity settings = scene->CreateEntity();
    scene->Add<MusicState>(settings, MusicState{.Track = track, .FadeSeconds = 0.0f, .Loop = true});

    AudioSystem system;
    ContextStorage storage;
    system.OnStart(*scene, storage.Make(engine));
    REQUIRE(engine.Music().GetVoiceCount() == 1);
    CHECK(engine.Music().Current().Get() == track.Get());
    const VoiceHandle voice = engine.Music().GetVoiceStates().front().Voice;

    // Ticking does not re-trigger the authored track: the same voice persists.
    for (int i = 0; i < 3; ++i)
    {
        system.OnUpdate(*scene, 1.0f / 60.0f, storage.Make(engine));
    }
    CHECK(engine.Music().GetVoiceCount() == 1);
    CHECK(engine.Music().GetVoiceStates().front().Voice == voice);
}
