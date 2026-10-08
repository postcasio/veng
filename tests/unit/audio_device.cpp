// The audio device contract, all provable without hardware because the null backend and the
// snapshot bridge are pure CPU: a null device satisfies the whole API, the bus tree composes gains
// as documented, the triple buffer is race-free by construction, the reclamation handshake waits
// for the mixing thread's generation to pass, the self-test tone is a bounded finite buffer, and a
// voice obeys its presentation scope: held is frozen, muted advances silently, closed stops.

#include <doctest/doctest.h>

#include <Veng/Audio/AudioBus.h>
#include <Veng/Audio/AudioBuffer.h>
#include <Veng/Audio/AudioDevice.h>
#include <Veng/Audio/AudioEngine.h>
#include <Veng/Audio/AudioGenerator.h>
#include <Veng/Audio/TripleBuffer.h>
#include <Veng/Audio/Voice.h>
#include "support/TestAudio.h"

#include <algorithm>
#include <atomic>
#include <cmath>
#include <initializer_list>
#include <span>
#include <thread>
#include <utility>
#include <vector>

using namespace Veng;
using namespace Veng::Audio;

namespace
{
    // A null device is what a headless / CI / cook run gets, and it runs the whole mix path on the
    // main thread, so every case below builds one.
    Unique<AudioDevice> MakeNullDevice()
    {
        return AudioDevice::Create(
            TestSupport::SharedPresentationScopes(),
            AudioDeviceInfo{.Backend = AudioBackend::Null, .SampleRate = 48000, .Channels = 2});
    }

    // A one-frame constant-amplitude mono buffer, looped, so the mixer reads a known value at every
    // cursor — the gain arithmetic is then the only thing the output can reflect.
    Ref<AudioBuffer> ConstantMono(f32 value, u32 frames, u32 sampleRate)
    {
        const std::vector<f32> samples(frames, value);
        return AudioBuffer::Create(samples, 1, sampleRate);
    }
}

TEST_CASE("null device satisfies the whole API")
{
    Unique<AudioDevice> device = MakeNullDevice();
    CHECK(device->IsNull());
    CHECK(device->GetSampleRate() == 48000);
    CHECK(device->GetChannels() == 2);

    AudioEngine& engine = device->GetEngine();
    for (const BusId bus : {AudioBuses::Master(), AudioBuses::Music(), AudioBuses::SFX(),
                            AudioBuses::UI(), AudioBuses::Ambience()})
    {
        engine.SetBusGain(bus, 0.75f);
        CHECK(engine.GetBusGain(bus) == doctest::Approx(0.75f));
    }

    const Ref<AudioBuffer> loop = ConstantMono(0.5f, 64, 48000);
    const VoiceHandle voice =
        engine.AddVoice(TestSupport::AppScope(), loop,
                        VoiceParams{.Bus = AudioBuses::SFX(), .Gain = 1.0f, .Loop = true});
    CHECK(voice.IsValid());
    CHECK(engine.IsVoiceLive(voice));
    CHECK(engine.GetActiveVoiceCount() == 1);

    // A pump publishes, advances the virtual clock, and drains — no call fails and the tracked
    // voice survives (it loops).
    device->Pump(1.0f / 60.0f);
    CHECK(engine.GetActiveVoiceCount() == 1);

    engine.StopVoice(voice);
    CHECK_FALSE(engine.IsVoiceLive(voice));
    device->Pump(1.0f / 60.0f);
    CHECK(engine.GetActiveVoiceCount() == 0);
}

TEST_CASE("a finite voice retires through the virtual clock")
{
    Unique<AudioDevice> device = MakeNullDevice();
    AudioEngine& engine = device->GetEngine();

    // A short one-shot: a tenth of a second, so a couple of frame pumps play it out.
    const std::vector<f32> samples(4800, 0.3f);
    const Ref<AudioBuffer> clip = AudioBuffer::Create(samples, 1, 48000);
    const VoiceHandle voice =
        engine.AddVoice(TestSupport::AppScope(), clip,
                        VoiceParams{.Bus = AudioBuses::SFX(), .Gain = 1.0f, .Loop = false});
    CHECK(engine.GetActiveVoiceCount() == 1);

    for (int i = 0; i < 30 && engine.GetActiveVoiceCount() > 0; ++i)
    {
        device->Pump(1.0f / 60.0f);
    }
    CHECK_FALSE(engine.IsVoiceLive(voice));
    CHECK(engine.GetActiveVoiceCount() == 0);
}

TEST_CASE("bus gain composes as documented")
{
    Unique<AudioDevice> device = MakeNullDevice();
    AudioEngine& engine = device->GetEngine();

    engine.SetBusGain(AudioBuses::Master(), 0.5f);
    engine.SetBusGain(AudioBuses::Music(), 0.5f);

    // Unit voice on the Music bus, panned hard left so the left channel carries the full pan gain:
    // 1.0 (sample) * 1.0 (voice) * 0.5 (Music) * 0.5 (Master) = 0.25, and nothing on the right.
    const Ref<AudioBuffer> loop = ConstantMono(1.0f, 64, 48000);
    engine.AddVoice(
        TestSupport::AppScope(), loop,
        VoiceParams{.Bus = AudioBuses::Music(), .Gain = 1.0f, .Pan = -1.0f, .Loop = true});
    engine.Publish();

    constexpr u32 frames = 128;
    std::vector<f32> output(static_cast<usize>(frames) * device->GetChannels(), 0.0f);
    device->RenderBlock(output, frames);

    CHECK(output[0] == doctest::Approx(0.25f).epsilon(0.001));
    CHECK(output[1] == doctest::Approx(0.0f).epsilon(0.001));
    CHECK(output[static_cast<usize>(frames - 1) * 2] == doctest::Approx(0.25f).epsilon(0.001));
}

TEST_CASE("the snapshot triple buffer is race-free by construction")
{
    // A payload whose two halves must always agree: the producer writes A then B, and a torn read
    // (or a buffer the writer reclaimed mid-read) would show them disagreeing.
    struct Payload
    {
        u64 A = 0;
        u64 B = 0;
    };

    TripleBuffer<Payload> buffer;
    std::atomic<bool> stop{false};
    std::atomic<u64> tornReads{0};
    std::atomic<u64> reads{0};

    std::thread consumer(
        [&]
        {
            u64 lastSeen = 0;
            while (!stop.load(std::memory_order_acquire))
            {
                if (buffer.FetchNewest())
                {
                    const Payload& front = buffer.FrontBuffer();
                    if (front.A != front.B)
                    {
                        tornReads.fetch_add(1, std::memory_order_relaxed);
                    }
                    // Serials only ever move forward: the consumer never latches an older frame
                    // than one it already saw.
                    if (front.A < lastSeen)
                    {
                        tornReads.fetch_add(1, std::memory_order_relaxed);
                    }
                    lastSeen = front.A;
                    reads.fetch_add(1, std::memory_order_relaxed);
                }
            }
        });

    constexpr u64 publishCount = 200000;
    for (u64 serial = 1; serial <= publishCount; ++serial)
    {
        Payload& back = buffer.BackBuffer();
        back.A = serial;
        back.B = serial;
        buffer.Publish();
    }
    stop.store(true, std::memory_order_release);
    consumer.join();

    CHECK(tornReads.load() == 0);
    CHECK(reads.load() > 0);
}

TEST_CASE("reclamation waits for the RT generation to pass")
{
    Unique<AudioDevice> device = MakeNullDevice();
    AudioEngine& engine = device->GetEngine();

    const Ref<AudioBuffer> resource = ConstantMono(0.4f, 64, 48000);
    CHECK(resource.use_count() == 1);

    const VoiceHandle voice =
        engine.AddVoice(TestSupport::AppScope(), resource,
                        VoiceParams{.Bus = AudioBuses::SFX(), .Gain = 1.0f, .Loop = true});
    CHECK(resource.use_count() == 2); // the engine's voice table holds it

    // Publish and mix once: the snapshot now references the resource, and the mixer's consumed
    // generation catches up to that publish.
    engine.Publish();
    std::vector<f32> output(128 * device->GetChannels(), 0.0f);
    device->RenderBlock(output, 128);
    const u64 referencingSerial = device->GetConsumedSerial();

    // Drop it from the live snapshot. It is not freed while the mixer's generation has not passed
    // the last snapshot that referenced it.
    engine.StopVoice(voice);
    engine.CollectDeferred();
    CHECK(device->GetConsumedSerial() == referencingSerial);
    CHECK(resource.use_count() == 2); // still held on the deferred-free queue

    // Republish (without the voice) and mix: the generation now passes the referencing snapshot,
    // and the resource is released.
    engine.Publish();
    device->RenderBlock(output, 128);
    CHECK(device->GetConsumedSerial() > referencingSerial);
    engine.CollectDeferred();
    CHECK(resource.use_count() == 1);
}

TEST_CASE("the self-test tone is a bounded finite buffer")
{
    const SelfTestTone tone = GenerateSelfTestTone(48000);
    CHECK(tone.Channels == 1);
    CHECK(tone.SampleRate == 48000);
    CHECK(tone.FrameCount == 12000); // 0.25 s at 48 kHz
    CHECK(tone.Samples.size() == tone.FrameCount);

    // A finite one-shot, never a runaway loop: its peak is bounded well under clipping.
    CHECK(tone.Peak > 0.0f);
    CHECK(tone.Peak <= 0.2f + 1e-4f);
    f32 measuredPeak = 0.0f;
    for (const f32 sample : tone.Samples)
    {
        measuredPeak = std::max(measuredPeak, std::abs(sample));
    }
    CHECK(measuredPeak == doctest::Approx(tone.Peak));
    CHECK(measuredPeak < 1.0f);
}

namespace
{
    // A null device judged by a registry the case owns, so the case latches scope states itself:
    // each Frame renews what the case says, latches, judges every voice, then mixes one 800-frame
    // block exactly as the application's presentation step and next frame's pump do.
    struct ScopedRig
    {
        PresentationScopes Scopes;
        Unique<AudioDevice> Device = AudioDevice::Create(
            Scopes,
            AudioDeviceInfo{.Backend = AudioBackend::Null, .SampleRate = 48000, .Channels = 2});

        AudioEngine& Engine() const { return Device->GetEngine(); }

        // Latches this frame's states (each scope in `renewed` renewed with its audibility), runs the
        // engine's once-per-frame update, and mixes the frame's samples.
        void Frame(std::initializer_list<std::pair<PresentationScope*, bool>> renewed)
        {
            for (const auto& [scope, audible] : renewed)
            {
                scope->Renew(audible);
            }
            Scopes.Resolve();
            Engine().Update(1.0f / 60.0f);
            Device->Pump(1.0f / 60.0f);
        }

        // The left channel of the last mixed block.
        vector<f32> Left() const
        {
            const std::span<const f32> mix = Device->GetDebugMixBuffer();
            vector<f32> left;
            for (usize i = 0; i < mix.size(); i += 2)
            {
                left.push_back(mix[i]);
            }
            return left;
        }
    };

    f32 PeakOf(const vector<f32>& samples)
    {
        f32 peak = 0.0f;
        for (const f32 s : samples)
        {
            peak = std::max(peak, std::abs(s));
        }
        return peak;
    }

    PresentationState StateOf(const AudioEngine& engine, const VoiceHandle voice)
    {
        for (const VoiceInfo& info : engine.GetVoiceInfos())
        {
            if (info.Handle == voice)
            {
                return info.State;
            }
        }
        return PresentationState::Closed;
    }
}

TEST_CASE("a held voice is frozen and resumes on the sample it held at")
{
    ScopedRig rig;
    const Unique<PresentationScope> scope = rig.Scopes.Open();

    // A looping rising ramp: any cursor advance while held would show as a jump on resume.
    constexpr u32 RampFrames = 48000;
    vector<f32> ramp(RampFrames);
    for (u32 i = 0; i < RampFrames; ++i)
    {
        ramp[i] = static_cast<f32>(i) / static_cast<f32>(RampFrames);
    }
    scope->Renew(true);
    rig.Scopes.Resolve();
    const VoiceHandle voice =
        rig.Engine().AddVoice(scope->GetId(), AudioBuffer::Create(ramp, 1, 48000),
                              VoiceParams{.Bus = AudioBuses::SFX(), .Gain = 1.0f, .Loop = true});
    REQUIRE(voice.IsValid());

    rig.Frame({{scope.get(), true}});
    const vector<f32> before = rig.Left();
    REQUIRE(before.size() >= 2);
    const f32 step = before.back() - before[before.size() - 2];
    REQUIRE(step > 0.0f);

    // Unrenewed, the scope is Held: the voice stays live, contributes nothing, and does not move.
    f32 heldPeak = 0.0f;
    for (int i = 0; i < 3; ++i)
    {
        rig.Frame({});
        heldPeak = std::max(heldPeak, PeakOf(rig.Left()));
    }
    CHECK(heldPeak == 0.0f);
    CHECK(rig.Engine().IsVoiceLive(voice));
    CHECK(StateOf(rig.Engine(), voice) == PresentationState::Held);

    // Renewed, the first sample continues the ramp from the one before the hold.
    rig.Frame({{scope.get(), true}});
    const vector<f32> after = rig.Left();
    REQUIRE_FALSE(after.empty());
    CHECK(after.front() == doctest::Approx(before.back() + step).epsilon(1e-4));
}

TEST_CASE("a muted voice is published silent and advances to its end")
{
    ScopedRig rig;
    const Unique<PresentationScope> scope = rig.Scopes.Open();
    scope->Renew(false);
    rig.Scopes.Resolve();

    // Half a block long, so a voice that advances retires within the first pump.
    const vector<f32> samples(400, 0.5f);
    const VoiceHandle voice =
        rig.Engine().AddVoice(scope->GetId(), AudioBuffer::Create(samples, 1, 48000),
                              VoiceParams{.Bus = AudioBuses::SFX(), .Gain = 1.0f, .Loop = false});
    REQUIRE(voice.IsValid());
    CHECK(StateOf(rig.Engine(), voice) == PresentationState::Muted);

    rig.Frame({{scope.get(), false}});
    CHECK(PeakOf(rig.Left()) == 0.0f);
    CHECK_FALSE(rig.Engine().IsVoiceLive(voice));
}

TEST_CASE("a closed scope stops every voice it owns and no other, and releases its sources")
{
    ScopedRig rig;
    Unique<PresentationScope> closing = rig.Scopes.Open();
    const Unique<PresentationScope> staying = rig.Scopes.Open();
    rig.Frame({{closing.get(), true}, {staying.get(), true}});

    struct Silence final : IAudioGenerator
    {
        void Render(f32* out, const u32 frames, const u32 channels, u32 /*sampleRate*/) override
        {
            std::fill_n(out, static_cast<usize>(frames) * channels, 0.0f);
        }
    };
    const Ref<AudioBuffer> loop = ConstantMono(0.1f, 64, 48000);
    const Ref<IAudioGenerator> generator = CreateRef<Silence>();
    const VoiceParams looping{.Bus = AudioBuses::SFX(), .Gain = 1.0f, .Loop = true};

    const VoiceHandle source = rig.Engine().AddVoice(closing->GetId(), loop, looping);
    const VoiceHandle generated = rig.Engine().PlayGenerator(
        closing->GetId(), generator, GeneratorVoiceParams{.Spatial = false});
    const VoiceHandle other = rig.Engine().AddVoice(staying->GetId(), loop, looping);
    REQUIRE(source.IsValid());
    REQUIRE(generated.IsValid());
    REQUIRE(other.IsValid());
    REQUIRE(generator.use_count() == 2);

    // The scene goes: the next update stops what its scope owned, whatever started it.
    const PresentationScopeId closedId = closing->GetId();
    closing.reset();
    rig.Frame({{staying.get(), true}});
    CHECK_FALSE(rig.Engine().IsVoiceLive(source));
    CHECK_FALSE(rig.Engine().IsVoiceLive(generated));
    CHECK(rig.Engine().IsVoiceLive(other));

    // After the reclamation handshake the engine holds no reference to the generator.
    for (int i = 0; i < 8 && rig.Engine().GetPendingReclaimCount() > 0; ++i)
    {
        rig.Frame({{staying.get(), true}});
    }
    CHECK(generator.use_count() == 1);

    // A voice cannot start in a scope that has closed.
    CHECK_FALSE(rig.Engine().AddVoice(closedId, loop, looping).IsValid());
}
