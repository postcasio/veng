// The code-facing audio surface over a null device — all pure CPU, no hardware. A one-shot lands in
// the snapshot then retires on its own or on demand; a PlayAt voice shares the spatialization path;
// the one-shot pool caps and drops the quietest; and the music director holds one logical track,
// crossfading equal-power between two and collapsing to one, playing the request of the
// highest-priority scope that is presented — running or paused — and following it as scopes close
// and requests change.

#include <doctest/doctest.h>

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
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneSystem.h>
#include "support/TestAudio.h"
#include "support/TestServices.h"

#include <algorithm>
#include <initializer_list>
#include <cstring>
#include <vector>

using namespace Veng;
using namespace Veng::Audio;

namespace
{
    Unique<AudioDevice> MakeNullDevice()
    {
        return AudioDevice::Create(
            TestSupport::SharedPresentationScopes(),
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

    // How a scope is presented in one MusicFrame: renewed audibly, renewed silently, or not renewed.
    enum class Shown : u8
    {
        Live,
        Muted,
        Held,
    };

    // One scope's presentation for one frame, and the rank of the viewport presenting it (none when
    // nothing does).
    struct ScopeShown
    {
        PresentationScope* Scope = nullptr;
        Shown How = Shown::Live;
        optional<u32> Rank;
    };

    ScopeShown Live(PresentationScope* scope, const optional<u32> rank = std::nullopt)
    {
        return ScopeShown{.Scope = scope, .How = Shown::Live, .Rank = rank};
    }

    ScopeShown Muted(PresentationScope* scope)
    {
        return ScopeShown{.Scope = scope, .How = Shown::Muted};
    }

    ScopeShown Held(PresentationScope* scope, const optional<u32> rank = std::nullopt)
    {
        return ScopeShown{.Scope = scope, .How = Shown::Held, .Rank = rank};
    }

    // One application frame's presentation step: renews and ranks the listed scopes, latches every
    // scope's state, then runs the engine's once-per-frame update (the music arbitration included).
    struct MusicFrame
    {
        PresentationScopes& Scopes;
        AudioEngine& Engine;

        void operator()(const std::initializer_list<ScopeShown> shown,
                        const f32 delta = 1.0f / 60.0f) const
        {
            for (const ScopeShown& entry : shown)
            {
                if (entry.How != Shown::Held)
                {
                    entry.Scope->Renew(entry.How == Shown::Live);
                }
                if (entry.Rank.has_value())
                {
                    Scopes.SetPresentationRank(entry.Scope->GetId(), *entry.Rank);
                }
            }
            Scopes.Resolve();
            Engine.Update(delta);
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
        engine.PlayOneShot(TestSupport::AppScope(), MakePcmClip(0.5f, 8),
                           OneShotParams{.Bus = AudioBuses::SFX(), .Gain = 0.8f});
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
    const VoiceHandle held = engine.PlayOneShot(TestSupport::AppScope(), MakePcmClip(0.5f, 48000),
                                                OneShotParams{.Loop = true});
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
    const VoiceHandle left = engine.PlayAt(TestSupport::AppScope(), clip, vec3(-10.0f, 0.0f, 0.0f),
                                           SpatialOneShotParams{.MaxDistance = 100.0f});
    REQUIRE(left.IsValid());
    const optional<VoiceParams> leftParams = engine.GetVoiceParams(left);
    REQUIRE(leftParams.has_value());
    CHECK(leftParams->Pan < -0.5f);

    const VoiceHandle right = engine.PlayAt(TestSupport::AppScope(), clip, vec3(10.0f, 0.0f, 0.0f),
                                            SpatialOneShotParams{.MaxDistance = 100.0f});
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
        handles.push_back(engine.PlayOneShot(TestSupport::AppScope(), clip,
                                             OneShotParams{.Gain = gain, .Loop = true}));
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
    const MusicDirector& music = engine.Music();
    const AssetHandle<AudioClip> trackA = MakePcmClip(0.5f, 48000);
    const AssetHandle<AudioClip> trackB = MakePcmClip(0.4f, 48000);
    const PresentationScopeId app = TestSupport::AppScope();

    // Hard-cut A in: it is the one logical track at full gain.
    engine.SetMusicRequest(app, MusicRequest{.Track = trackA, .FadeSeconds = 0.0f});
    engine.Update(0.0f);
    REQUIRE(music.GetVoiceCount() == 1);
    CHECK(music.Current().Get() == trackA.Get());
    const VoiceHandle aVoice = music.GetVoiceStates().front().Voice;

    // Requesting A again at another fade is a no-op: no re-trigger, no second voice.
    engine.SetMusicRequest(app, MusicRequest{.Track = trackA, .FadeSeconds = 0.5f});
    engine.Update(0.0f);
    CHECK(music.GetVoiceCount() == 1);
    CHECK(music.GetVoiceStates().front().Voice == aVoice);

    // Crossfade to B over one second. At the start A is full, B silent.
    engine.SetMusicRequest(app, MusicRequest{.Track = trackB, .FadeSeconds = 1.0f});
    engine.Update(0.0f);
    REQUIRE(music.GetVoiceCount() == 2);
    CHECK(music.Current().Get() == trackB.Get());
    const vector<MusicDirector::VoiceState> atStart = music.GetVoiceStates();
    CHECK(GainOf(atStart, true) == doctest::Approx(1.0f));
    CHECK(GainOf(atStart, false) == doctest::Approx(0.0f));

    // Midpoint: equal-power means both sit at cos(pi/4) and the power sum stays unity.
    engine.Update(0.5f);
    const vector<MusicDirector::VoiceState> atMid = music.GetVoiceStates();
    const f32 outMid = GainOf(atMid, true);
    const f32 inMid = GainOf(atMid, false);
    CHECK(outMid == doctest::Approx(0.70710677f).epsilon(0.01f));
    CHECK(inMid == doctest::Approx(0.70710677f).epsilon(0.01f));
    CHECK(outMid * outMid + inMid * inMid == doctest::Approx(1.0f).epsilon(0.01f));

    // After the full fade exactly one voice remains: B, no longer fading.
    engine.Update(0.5f);
    CHECK(music.GetVoiceCount() == 1);
    CHECK(music.Current().Get() == trackB.Get());
    const vector<MusicDirector::VoiceState> atEnd = music.GetVoiceStates();
    REQUIRE(atEnd.size() == 1);
    CHECK_FALSE(atEnd.front().FadingOut);
    CHECK(atEnd.front().Gain == doctest::Approx(1.0f));
}

TEST_CASE("withdrawing the last request fades the music out over its fade")
{
    const Unique<AudioDevice> device = MakeNullDevice();
    AudioEngine& engine = device->GetEngine();
    const MusicDirector& music = engine.Music();
    const PresentationScopeId app = TestSupport::AppScope();

    engine.SetMusicRequest(app,
                           MusicRequest{.Track = MakePcmClip(0.5f, 48000), .FadeSeconds = 0.5f});
    engine.Update(0.5f);
    REQUIRE(music.GetVoiceCount() == 1);

    engine.SetMusicRequest(app, std::nullopt);
    engine.Update(0.25f);
    CHECK_FALSE(music.Current().IsValid());
    CHECK_FALSE(engine.GetMusicWinner().IsValid());
    CHECK(music.GetVoiceCount() == 1);
    engine.Update(0.25f);
    CHECK(music.GetVoiceCount() == 0);
}

TEST_CASE("the highest-priority music request plays, and swapping priorities crossfades")
{
    TestSupport::TestServices services;
    PresentationScopes& scopes = services.GetPresentationScopes();
    AudioEngine& engine = services.GetAudio();
    const Unique<PresentationScope> first = scopes.Open();
    const Unique<PresentationScope> second = scopes.Open();
    const AssetHandle<AudioClip> firstTrack = MakePcmClip(0.5f, 48000);
    const AssetHandle<AudioClip> secondTrack = MakePcmClip(0.4f, 48000);
    const MusicFrame frame{.Scopes = scopes, .Engine = engine};

    engine.SetMusicRequest(first->GetId(),
                           MusicRequest{.Track = firstTrack, .FadeSeconds = 0.5f, .Priority = 0});
    engine.SetMusicRequest(second->GetId(),
                           MusicRequest{.Track = secondTrack, .FadeSeconds = 0.5f, .Priority = 1});
    frame({Live(first.get()), Live(second.get())});
    CHECK(engine.Music().Current().Get() == secondTrack.Get());
    CHECK(engine.GetMusicWinner() == second->GetId());

    engine.SetMusicRequest(first->GetId(),
                           MusicRequest{.Track = firstTrack, .FadeSeconds = 0.5f, .Priority = 2});
    frame({Live(first.get()), Live(second.get())});
    CHECK(engine.Music().Current().Get() == firstTrack.Get());
    CHECK(engine.Music().GetVoiceCount() == 2);
}

TEST_CASE("at equal priority the scene on the earlier viewport takes the music")
{
    TestSupport::TestServices services;
    PresentationScopes& scopes = services.GetPresentationScopes();
    AudioEngine& engine = services.GetAudio();
    const Unique<PresentationScope> first = scopes.Open();
    const Unique<PresentationScope> second = scopes.Open();
    const AssetHandle<AudioClip> firstTrack = MakePcmClip(0.5f, 48000);
    const AssetHandle<AudioClip> secondTrack = MakePcmClip(0.4f, 48000);
    const MusicFrame frame{.Scopes = scopes, .Engine = engine};
    engine.SetMusicRequest(first->GetId(), MusicRequest{.Track = firstTrack});
    engine.SetMusicRequest(second->GetId(), MusicRequest{.Track = secondTrack});

    frame({Live(first.get(), 1), Live(second.get(), 0)});
    CHECK(engine.Music().Current().Get() == secondTrack.Get());

    frame({Live(first.get(), 0), Live(second.get(), 1)});
    CHECK(engine.Music().Current().Get() == firstTrack.Get());

    // A ranked scene beats one with no rank, however old.
    frame({Live(first.get()), Live(second.get(), 3)});
    CHECK(engine.Music().Current().Get() == secondTrack.Get());
}

TEST_CASE("closing the winning scope returns the music to the next request, then fades it out")
{
    TestSupport::TestServices services;
    PresentationScopes& scopes = services.GetPresentationScopes();
    AudioEngine& engine = services.GetAudio();
    Unique<PresentationScope> winner = scopes.Open();
    Unique<PresentationScope> runnerUp = scopes.Open();
    const AssetHandle<AudioClip> winnerTrack = MakePcmClip(0.5f, 48000);
    const AssetHandle<AudioClip> runnerUpTrack = MakePcmClip(0.4f, 48000);
    const MusicFrame frame{.Scopes = scopes, .Engine = engine};
    constexpr f32 Fade = 0.25f;
    engine.SetMusicRequest(winner->GetId(),
                           MusicRequest{.Track = winnerTrack, .FadeSeconds = Fade, .Priority = 1});
    engine.SetMusicRequest(runnerUp->GetId(),
                           MusicRequest{.Track = runnerUpTrack, .FadeSeconds = Fade});
    frame({Live(winner.get()), Live(runnerUp.get())});
    REQUIRE(engine.Music().Current().Get() == winnerTrack.Get());

    winner.reset();
    frame({Live(runnerUp.get())});
    CHECK(engine.Music().Current().Get() == runnerUpTrack.Get());
    CHECK(engine.GetMusicRequests().size() == 1);

    runnerUp.reset();
    frame({}, Fade);
    frame({}, Fade);
    CHECK_FALSE(engine.Music().Current().IsValid());
    CHECK(engine.Music().GetVoiceCount() == 0);
    CHECK(engine.GetMusicRequests().empty());
}

TEST_CASE("a scene nothing presents never takes the music")
{
    TestSupport::TestServices services;
    PresentationScopes& scopes = services.GetPresentationScopes();
    AudioEngine& engine = services.GetAudio();
    Unique<PresentationScope> shown = scopes.Open();
    const Unique<PresentationScope> muted = scopes.Open();
    const Unique<PresentationScope> heldOffscreen = scopes.Open();
    const AssetHandle<AudioClip> shownTrack = MakePcmClip(0.5f, 48000);
    const MusicFrame frame{.Scopes = scopes, .Engine = engine};
    engine.SetMusicRequest(shown->GetId(), MusicRequest{.Track = shownTrack});
    engine.SetMusicRequest(muted->GetId(),
                           MusicRequest{.Track = MakePcmClip(0.4f, 48000), .Priority = 100});
    engine.SetMusicRequest(heldOffscreen->GetId(),
                           MusicRequest{.Track = MakePcmClip(0.3f, 48000), .Priority = 100});

    frame({Live(shown.get()), Muted(muted.get())});
    CHECK(engine.Music().Current().Get() == shownTrack.Get());

    // Alone, neither a muted scene nor a held one with no presentation rank plays at all.
    shown.reset();
    frame({Muted(muted.get())});
    CHECK_FALSE(engine.Music().Current().IsValid());
    CHECK(engine.Music().GetVoiceCount() == 0);
}

TEST_CASE("a paused scene still on screen keeps its music without renewing its request")
{
    TestSupport::TestServices services;
    PresentationScopes& scopes = services.GetPresentationScopes();
    AudioEngine& engine = services.GetAudio();
    const Unique<PresentationScope> scope = scopes.Open();
    const AssetHandle<AudioClip> track = MakePcmClip(0.5f, 48000);
    const MusicFrame frame{.Scopes = scopes, .Engine = engine};
    engine.SetMusicRequest(scope->GetId(), MusicRequest{.Track = track});
    frame({Live(scope.get(), 0)});
    REQUIRE(engine.Music().GetVoiceCount() == 1);
    const VoiceHandle voice = engine.Music().GetVoiceStates().front().Voice;

    for (int i = 0; i < 3; ++i)
    {
        frame({Held(scope.get(), 0)});
    }
    REQUIRE(scopes.GetState(scope->GetId()) == PresentationState::Held);
    CHECK(engine.GetMusicWinner() == scope->GetId());
    REQUIRE(engine.Music().GetVoiceCount() == 1);
    CHECK(engine.Music().GetVoiceStates().front().Voice == voice);
}

TEST_CASE("a runtime MusicState edit crossfades, and an unchanged one never re-triggers")
{
    TypeRegistry registry;
    RegisterBuiltinTypes(registry);
    TestSupport::TestServices services;
    AudioEngine& engine = services.GetAudio();
    const Unique<Scene> scene = Scene::Create(registry);
    scene->SetPresentationScope(services.GetPresentationScopes().Open());
    const SystemContext context = services.Make(SystemContextRequest{
        .World = WorldInstanceId{1}, .Scene = *scene, .Phase = SystemContextPhase::View});
    const MusicFrame frame{.Scopes = services.GetPresentationScopes(), .Engine = engine};
    const AssetHandle<AudioClip> first = MakePcmClip(0.5f, 48000);
    const AssetHandle<AudioClip> second = MakePcmClip(0.4f, 48000);
    const Entity settings = scene->CreateEntity();
    scene->Add<MusicState>(settings, MusicState{.Track = first, .FadeSeconds = 0.5f});

    AudioSystem system;
    system.OnStart(*scene, context);
    const auto update = [&]
    {
        system.OnUpdate(*scene, 1.0f / 60.0f, context);
        frame({Live(scene->GetPresentationScope())});
    };
    update();
    REQUIRE(engine.Music().Current().Get() == first.Get());
    const VoiceHandle voice = engine.Music().GetVoiceStates().back().Voice;
    for (int i = 0; i < 3; ++i)
    {
        update();
    }
    CHECK(engine.Music().GetVoiceStates().back().Voice == voice);

    scene->Get<MusicState>(settings).Track = second;
    update();
    CHECK(engine.Music().Current().Get() == second.Get());
    CHECK(engine.Music().GetVoiceCount() == 2);

    // Removing the component withdraws the request.
    REQUIRE(scene->Remove<MusicState>(settings));
    update();
    CHECK(engine.GetMusicRequests().empty());
    CHECK_FALSE(engine.Music().Current().IsValid());
    system.OnStop(*scene, context);
}

TEST_CASE("a replay-built facade starts nothing, and still controls a live voice")
{
    const Unique<AudioDevice> device = MakeNullDevice();
    AudioEngine& engine = device->GetEngine();
    const AssetHandle<AudioClip> clip = MakePcmClip(0.5f, 48000);

    struct Silence final : IAudioGenerator
    {
        void Render(f32* out, const u32 frames, const u32 channels, u32 /*sampleRate*/) override
        {
            std::fill_n(out, static_cast<usize>(frames) * channels, 0.0f);
        }
    };

    // A reconciliation replay re-runs a tick whose sound already played: every start is refused.
    const ScopedAudio replay(engine, TestSupport::AppScope(), true);
    CHECK_FALSE(replay.PlayOneShot(clip).IsValid());
    CHECK_FALSE(replay.PlayAt(clip, vec3(0.0f)).IsValid());
    CHECK_FALSE(replay.AddClipVoice(clip, VoiceParams{}).IsValid());
    CHECK_FALSE(replay.PlayGenerator(CreateRef<Silence>(), GeneratorVoiceParams{}).IsValid());
    // And so is every start through a facade bound to no engine.
    CHECK_FALSE(ScopedAudio::Unbound().PlayOneShot(clip).IsValid());
    CHECK(engine.GetActiveVoiceCount() == 0);

    // Controls on what is already playing work as usual inside a replay.
    const VoiceHandle voice =
        ScopedAudio(engine, TestSupport::AppScope(), false).PlayOneShot(clip, {.Loop = true});
    REQUIRE(voice.IsValid());
    CHECK(replay.IsVoiceLive(voice));
    replay.StopVoice(voice);
    CHECK_FALSE(engine.IsVoiceLive(voice));
}
