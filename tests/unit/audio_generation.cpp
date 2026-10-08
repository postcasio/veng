// Runtime-generated audio over a null device, all pure CPU. CreateClip round-trips a code-built
// buffer into an ordinary playable clip; a generator fills exactly the samples it is asked for with
// no seam across block boundaries; a GeneratorParams block crosses the thread boundary without
// tearing; a generator's Render is deterministic given its params; and a spatial generator is
// placed and panned through the very same path a clip is, with no generator-specific code.

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Audio/AudioBuffer.h>
#include <Veng/Audio/AudioClip.h>
#include <Veng/Audio/AudioDevice.h>
#include <Veng/Audio/AudioEngine.h>
#include <Veng/Audio/AudioGenerator.h>
#include "support/TestAudio.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <numbers>
#include <span>
#include <thread>
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

    // A known mono sine — the "built in code" buffer CreateClip must round-trip exactly.
    std::vector<f32> MakeSine(const u32 frames, const f32 frequency, const u32 sampleRate)
    {
        std::vector<f32> samples(frames);
        for (u32 i = 0; i < frames; ++i)
        {
            samples[i] = 0.5f * std::sin(2.0f * std::numbers::pi_v<f32> * frequency *
                                         static_cast<f32>(i) / static_cast<f32>(sampleRate));
        }
        return samples;
    }

    // A generator that writes a monotonically increasing ramp, so a gap or overlap at a block seam
    // shows up as a break in the sequence. It maintains its own position across Render calls.
    struct RampGenerator final : IAudioGenerator
    {
        f64 Next = 0.0;
        void Render(f32* out, const u32 frames, const u32 channels, u32 /*sampleRate*/) override
        {
            for (u32 i = 0; i < frames * channels; ++i)
            {
                out[i] = static_cast<f32>(Next);
                Next += 1.0;
            }
        }
    };

    // A generator whose per-sample increment is driven live through a GeneratorParams block; its
    // output depends only on that param and the block sequence, so two identically-driven instances
    // agree sample-for-sample.
    struct StepGenerator final : IAudioGenerator
    {
        struct Drive
        {
            f32 Step = 0.0f;
        };
        GeneratorParams<Drive> Params;
        f64 Accumulator = 0.0;

        void Render(f32* out, const u32 frames, const u32 channels, u32 /*sampleRate*/) override
        {
            const Drive drive = Params.Get();
            for (u32 i = 0; i < frames * channels; ++i)
            {
                out[i] = static_cast<f32>(Accumulator);
                Accumulator += static_cast<f64>(drive.Step);
            }
        }
    };

    // Counts a LifetimeGenerator's destructions, and the thread the last one ran on.
    struct LifetimeProbe
    {
        int Destroyed = 0;
        std::thread::id DestroyedOn;
    };

    // A generator that reports its destruction to a probe the test owns, so the test can watch when
    // (and where) the engine's reference is the last to go.
    struct LifetimeGenerator final : IAudioGenerator
    {
        explicit LifetimeGenerator(LifetimeProbe& probe) : Probe(probe) {}
        ~LifetimeGenerator() override
        {
            ++Probe.Destroyed;
            Probe.DestroyedOn = std::this_thread::get_id();
        }
        LifetimeGenerator(const LifetimeGenerator&) = delete;
        LifetimeGenerator& operator=(const LifetimeGenerator&) = delete;
        LifetimeGenerator(LifetimeGenerator&&) = delete;
        LifetimeGenerator& operator=(LifetimeGenerator&&) = delete;

        void Render(f32* out, const u32 frames, const u32 channels, u32 /*sampleRate*/) override
        {
            std::fill_n(out, static_cast<usize>(frames) * channels, 0.25f);
        }

        LifetimeProbe& Probe;
    };

    // A generator emitting a constant, so a mix is trivially non-silent while it is live.
    struct ConstantGenerator final : IAudioGenerator
    {
        f32 Value = 1.0f;
        void Render(f32* out, const u32 frames, const u32 channels, u32 /*sampleRate*/) override
        {
            for (u32 i = 0; i < frames * channels; ++i)
            {
                out[i] = Value;
            }
        }
    };
}

TEST_CASE("CreateClip wraps a code-built buffer as an ordinary, playable clip")
{
    const Unique<AudioDevice> device = MakeNullDevice();
    AudioEngine& engine = device->GetEngine();

    const u32 frames = 16;
    const std::vector<f32> sine = MakeSine(frames, 1000.0f, 48000);
    const AssetHandle<AudioClip> clip =
        engine.CreateClip(sine, AudioBufferFormat{.SampleRate = 48000, .Channels = 1});

    // The handle resolves to a resident Pcm clip carrying exactly the built samples.
    const AudioClip* resolved = clip.Get();
    REQUIRE(resolved != nullptr);
    CHECK(resolved->Storage() == AudioStorage::Pcm);
    CHECK(resolved->SampleRate() == 48000);
    CHECK(resolved->Channels() == 1);
    CHECK(resolved->FrameCount() == frames);
    REQUIRE(resolved->Buffer() != nullptr);
    const std::span<const f32> stored = resolved->Buffer()->Samples();
    REQUIRE(stored.size() == sine.size());
    f32 maxDiff = 0.0f;
    for (usize i = 0; i < sine.size(); ++i)
    {
        maxDiff = std::max(maxDiff, std::abs(stored[i] - sine[i]));
    }
    CHECK(maxDiff == 0.0f);

    // It plays through the ordinary voice path: a non-looping voice retires after its duration.
    const VoiceHandle once =
        engine.PlayOneShot(TestSupport::AppScope(), clip, OneShotParams{.Loop = false});
    REQUIRE(once.IsValid());
    for (int i = 0; i < 4 && engine.IsVoiceLive(once); ++i)
    {
        device->Pump(1.0f / 60.0f);
    }
    CHECK_FALSE(engine.IsVoiceLive(once));

    // A looping voice on the same clip is still live long past its 16-frame duration.
    const VoiceHandle looping =
        engine.PlayOneShot(TestSupport::AppScope(), clip, OneShotParams{.Loop = true});
    REQUIRE(looping.IsValid());
    for (int i = 0; i < 4; ++i)
    {
        device->Pump(1.0f / 60.0f);
    }
    CHECK(engine.IsVoiceLive(looping));
}

TEST_CASE("a generator fills exactly what it is asked with no seam across blocks")
{
    RampGenerator generator;
    std::vector<f32> collected;
    for (const u32 block : {3U, 7U, 1U, 16U, 5U, 9U})
    {
        std::vector<f32> buffer(block, -1.0f);
        generator.Render(buffer.data(), block, 1, 48000);
        collected.insert(collected.end(), buffer.begin(), buffer.end());
    }

    // The concatenation of the varying blocks is one unbroken 0, 1, 2, ... ramp: a break at any
    // seam (a repeated or skipped sample) is a mismatch. Assert the aggregate, not each element.
    u32 mismatches = 0;
    for (usize i = 0; i < collected.size(); ++i)
    {
        if (collected[i] != static_cast<f32>(i))
        {
            ++mismatches;
        }
    }
    CHECK(collected.size() == 41);
    CHECK(mismatches == 0);
}

TEST_CASE("GeneratorParams publishes across threads without tearing")
{
    struct Pair
    {
        u32 A = 0;
        u32 B = 0;
    };
    GeneratorParams<Pair> params;
    params.Set(Pair{.A = 0, .B = 0});

    std::atomic<bool> stop{false};
    std::atomic<u64> reads{0};
    std::atomic<u64> torn{0};

    // Reader latches the newest published pair and checks the invariant A == B. The writer only ever
    // publishes pairs that satisfy it, so any A != B observed would be a torn read across the seam.
    std::thread reader(
        [&]
        {
            while (!stop.load(std::memory_order_acquire))
            {
                const Pair p = params.Get();
                if (p.A != p.B)
                {
                    torn.fetch_add(1, std::memory_order_relaxed);
                }
                reads.fetch_add(1, std::memory_order_relaxed);
            }
        });

    for (u32 i = 1; i <= 200000; ++i)
    {
        params.Set(Pair{.A = i, .B = i});
    }
    // Let the reader run against the settled value a moment, then join.
    while (reads.load(std::memory_order_relaxed) < 1000)
    {
        std::this_thread::yield();
    }
    stop.store(true, std::memory_order_release);
    reader.join();

    CHECK(reads.load() > 0);
    CHECK(torn.load() == 0);
}

TEST_CASE("a generator's Render is deterministic given its params")
{
    StepGenerator a;
    StepGenerator b;
    a.Params.Set(StepGenerator::Drive{.Step = 0.25f});
    b.Params.Set(StepGenerator::Drive{.Step = 0.25f});

    std::vector<f32> outA;
    std::vector<f32> outB;
    for (const u32 block : {5U, 11U, 2U, 8U})
    {
        std::vector<f32> bufA(block);
        std::vector<f32> bufB(block);
        a.Render(bufA.data(), block, 1, 48000);
        b.Render(bufB.data(), block, 1, 48000);
        outA.insert(outA.end(), bufA.begin(), bufA.end());
        outB.insert(outB.end(), bufB.begin(), bufB.end());
    }

    REQUIRE(outA.size() == outB.size());
    f32 maxDiff = 0.0f;
    for (usize i = 0; i < outA.size(); ++i)
    {
        maxDiff = std::max(maxDiff, std::abs(outA[i] - outB[i]));
    }
    CHECK(maxDiff == 0.0f);
}

TEST_CASE("a spatial generator is placed and panned through the clip spatialization path")
{
    const Unique<AudioDevice> device = MakeNullDevice();
    AudioEngine& engine = device->GetEngine();
    const auto generator = CreateRef<ConstantGenerator>();

    // Hard-left of a listener at the origin (identity rotation, +X right): the voice pans left,
    // using the same StereoPan a PlayAt clip does — no generator-specific spatialization exists.
    const VoiceHandle voice = engine.PlayGenerator(
        TestSupport::AppScope(), generator,
        GeneratorVoiceParams{
            .Spatial = true, .Position = vec3(-10.0f, 0.0f, 0.0f), .MaxDistance = 100.0f});
    REQUIRE(voice.IsValid());
    const optional<VoiceParams> params = engine.GetVoiceParams(voice);
    REQUIRE(params.has_value());
    CHECK(params->Pan < -0.5f);

    // SetVoicePose (the shared moving-emitter path) moves it hard-right; the pan follows.
    engine.SetVoicePose(voice, vec3(10.0f, 0.0f, 0.0f), vec3(0.0f));
    CHECK(engine.GetVoiceParams(voice)->Pan > 0.5f);

    engine.StopVoice(voice);
    CHECK_FALSE(engine.IsVoiceLive(voice));
    CHECK(engine.GetActiveVoiceCount() == 0);
}

TEST_CASE("a generator voice mixes through the null device and StopVoice reclaims it")
{
    const Unique<AudioDevice> device = MakeNullDevice();
    AudioEngine& engine = device->GetEngine();
    const auto generator = CreateRef<ConstantGenerator>();
    generator->Value = 0.5f;

    const VoiceHandle voice = engine.PlayGenerator(
        TestSupport::AppScope(), generator,
        GeneratorVoiceParams{.Bus = AudioBuses::Master(), .Spatial = false, .Gain = 1.0f});
    REQUIRE(voice.IsValid());
    CHECK(engine.GetActiveVoiceCount() == 1);

    device->Pump(1.0f / 60.0f);
    // The constant generator drives the master bus, so the mixed block is audibly non-zero.
    f32 peak = 0.0f;
    for (const f32 sample : device->GetDebugMixBuffer())
    {
        peak = std::max(peak, std::abs(sample));
    }
    CHECK(peak > 0.1f);

    engine.StopVoice(voice);
    CHECK_FALSE(engine.IsVoiceLive(voice));
    CHECK(engine.GetActiveVoiceCount() == 0);
}

TEST_CASE("stopping generator voices waits for no mixer frame and reclaims them on the next pump")
{
    // Declared before the device, so it outlives any generator the engine still holds at teardown.
    LifetimeProbe probe;
    const Unique<AudioDevice> device = MakeNullDevice();
    AudioEngine& engine = device->GetEngine();

    // The engine's references are the only ones: each generator lives exactly as long as its voice
    // needs it.
    constexpr int VoiceCount = 3;
    std::vector<VoiceHandle> voices;
    for (int i = 0; i < VoiceCount; ++i)
    {
        voices.push_back(engine.PlayGenerator(
            TestSupport::AppScope(), CreateRef<LifetimeGenerator>(probe),
            GeneratorVoiceParams{.Bus = AudioBuses::Master(), .Spatial = false, .Gain = 0.3f}));
        REQUIRE(voices.back().IsValid());
    }
    device->Pump(1.0f / 60.0f);

    // On the null device a stop that waited would have to mix inline to advance the consumed serial;
    // an unmoved serial shows none of the stops waited. The generators outlive the stop, held by the
    // engine for the snapshot the mixer may still be reading.
    const u64 consumedBeforeStop = device->GetConsumedSerial();
    for (const VoiceHandle voice : voices)
    {
        engine.StopVoice(voice);
    }
    CHECK(device->GetConsumedSerial() == consumedBeforeStop);
    CHECK(engine.GetActiveVoiceCount() == 0);
    CHECK(engine.GetPendingReclaimCount() == VoiceCount);
    CHECK(probe.Destroyed == 0);

    // One pump publishes a frame naming none of them and mixes it, which is the mixer provably past
    // every one: all are destroyed in that pump, on this (the main) thread.
    device->Pump(1.0f / 60.0f);
    CHECK(engine.GetPendingReclaimCount() == 0);
    CHECK(probe.Destroyed == VoiceCount);
    CHECK(probe.DestroyedOn == std::this_thread::get_id());
}

TEST_CASE("a caller's own reference keeps a generator alive past the voice's reclamation")
{
    // Declared before the device, so it outlives any generator the engine still holds at teardown.
    LifetimeProbe probe;
    const Unique<AudioDevice> device = MakeNullDevice();
    AudioEngine& engine = device->GetEngine();
    auto generator = CreateRef<LifetimeGenerator>(probe);

    const VoiceHandle voice =
        engine.PlayGenerator(TestSupport::AppScope(), generator,
                             GeneratorVoiceParams{.Bus = AudioBuses::Master(), .Spatial = false});
    REQUIRE(voice.IsValid());
    device->Pump(1.0f / 60.0f);

    engine.StopVoice(voice);
    device->Pump(1.0f / 60.0f);
    REQUIRE(engine.GetPendingReclaimCount() == 0);
    CHECK(probe.Destroyed == 0);
    CHECK(generator.use_count() == 1);

    generator.reset();
    CHECK(probe.Destroyed == 1);
}

TEST_CASE("stopping a dead or invalid handle is a no-op")
{
    // Declared before the device, so it outlives any generator the engine still holds at teardown.
    LifetimeProbe probe;
    const Unique<AudioDevice> device = MakeNullDevice();
    AudioEngine& engine = device->GetEngine();

    const VoiceHandle voice =
        engine.PlayGenerator(TestSupport::AppScope(), CreateRef<LifetimeGenerator>(probe),
                             GeneratorVoiceParams{.Bus = AudioBuses::Master(), .Spatial = false});
    REQUIRE(voice.IsValid());
    const VoiceHandle survivor =
        engine.PlayGenerator(TestSupport::AppScope(), CreateRef<LifetimeGenerator>(probe),
                             GeneratorVoiceParams{.Bus = AudioBuses::Master(), .Spatial = false});
    REQUIRE(survivor.IsValid());

    engine.StopVoice(voice);
    REQUIRE(engine.GetPendingReclaimCount() == 1);

    // Neither a stale handle nor a default one touches the live voice or queues a second reclaim.
    engine.StopVoice(voice);
    engine.StopVoice(VoiceHandle{});
    CHECK(engine.GetPendingReclaimCount() == 1);
    CHECK(engine.IsVoiceLive(survivor));
    CHECK(engine.GetActiveVoiceCount() == 1);

    device->Pump(1.0f / 60.0f);
    CHECK(probe.Destroyed == 1);
}
