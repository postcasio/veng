// Buffered generator voices: an IAudioGenerator rendered ahead of time on the audio fill thread into
// a ring the real-time callback only drains, so heavy, latency-tolerant synthesis never runs on — and
// so can never underrun — the real-time thread. Mirrors the streaming-voice tests: a buffered voice
// eventually plays exactly what the generator produces (past the initial fill latency), a
// Buffered && Spatial request is rejected, StopVoice blocks until neither the mixer nor the fill
// thread can reach the borrowed generator (so it is free to destroy the moment StopVoice returns), and
// an unfilled ring drains as silence rather than a hang or garbage. All pure CPU over the
// null-but-computing device.

#include <doctest/doctest.h>

#include <Veng/Audio/AudioDevice.h>
#include <Veng/Audio/AudioEngine.h>
#include <Veng/Audio/AudioGenerator.h>

#include <algorithm>
#include <atomic>
#include <chrono>
#include <cmath>
#include <span>
#include <thread>
#include <vector>

using namespace Veng;
using namespace Veng::Audio;

namespace
{
    Unique<AudioDevice> MakeStereoNullDevice()
    {
        return AudioDevice::Create(
            AudioDeviceInfo{.Backend = AudioBackend::Null, .SampleRate = 48000, .Channels = 2});
    }

    // A stereo generator writing one monotonically increasing counter to both channels, starting at 1
    // so the first rendered sample is distinguishable from underrun silence. A gap or duplicate at a
    // ring seam shows up as a break in the drained sequence. It counts its Render calls so a test can
    // prove the fill thread stopped rendering it the instant StopVoice returned.
    struct StereoRampGenerator final : IAudioGenerator
    {
        f64 Next = 1.0;
        std::atomic<u64> RenderCalls{0};
        void Render(f32* out, const u32 frames, const u32 channels, u32 /*sampleRate*/) override
        {
            RenderCalls.fetch_add(1, std::memory_order_relaxed);
            for (u32 f = 0; f < frames; ++f)
            {
                const f32 v = static_cast<f32>(Next);
                for (u32 c = 0; c < channels; ++c)
                {
                    out[f * channels + c] = v;
                }
                Next += 1.0;
            }
        }
    };

    // A generator emitting a constant, so a mix is trivially non-silent and always in range.
    struct ConstantGenerator final : IAudioGenerator
    {
        f32 Value = 0.5f;
        void Render(f32* out, const u32 frames, const u32 channels, u32 /*sampleRate*/) override
        {
            for (u32 i = 0; i < frames * channels; ++i)
            {
                out[i] = Value;
            }
        }
    };

    // Pumps the null device until it has emitted at least wantSamples interleaved samples, giving the
    // fill thread a moment between pumps to top the ring. Returns the accumulated interleaved output.
    std::vector<f32> Capture(AudioDevice& device, const usize wantSamples)
    {
        std::vector<f32> out;
        for (int i = 0; i < 400 && out.size() < wantSamples; ++i)
        {
            device.Pump(0.02f);
            const std::span<const f32> buffer = device.GetDebugMixBuffer();
            out.insert(out.end(), buffer.begin(), buffer.end());
            std::this_thread::sleep_for(std::chrono::milliseconds(1));
        }
        return out;
    }
}

TEST_CASE("a buffered generator eventually plays exactly what it produces, contiguously")
{
    const Unique<AudioDevice> device = MakeStereoNullDevice();
    AudioEngine& engine = device->GetEngine();
    StereoRampGenerator generator;

    const VoiceHandle voice = engine.PlayGenerator(
        &generator, GeneratorVoiceParams{
                        .Bus = AudioBus::Master, .Channels = 2, .Buffered = true, .Gain = 1.0f});
    REQUIRE(voice.IsValid());

    // Give the fill thread time to render the head of the ramp into the ring before draining begins.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    const std::vector<f32> out = Capture(*device, 4000);
    REQUIRE(out.size() >= 4000);

    // The left channel: unity master gain, no bus low-pass, no reverb send, and a stereo image
    // bypasses the pan — so out[even] is the generator's value verbatim. Skip the initial underrun
    // silence (zeros before the first rendered sample lands), then the remainder must be the unbroken
    // ramp 1, 2, 3, ... the generator wrote: a dropped or repeated sample at any ring seam breaks it.
    std::vector<f32> left;
    for (usize i = 0; i < out.size(); i += 2)
    {
        left.push_back(out[i]);
    }
    usize start = 0;
    while (start < left.size() && left[start] == 0.0f)
    {
        ++start;
    }
    REQUIRE(start < left.size());                // real signal arrived past the fill latency
    CHECK(left[start] == doctest::Approx(1.0f)); // draining begins at the ramp's first sample

    u32 breaks = 0;
    f32 peak = 0.0f;
    for (usize i = start + 1; i < left.size(); ++i)
    {
        if (left[i] != doctest::Approx(left[i - 1] + 1.0f))
        {
            ++breaks;
        }
        peak = std::max(peak, std::abs(left[i]));
    }
    CHECK(breaks == 0); // one contiguous ramp: no gap, no duplicate across any ring seam
    CHECK(peak > 1.0f); // the voice actually played, not a single stray sample
}

TEST_CASE("a buffered spatial generator request is rejected; a non-spatial one is accepted")
{
    const Unique<AudioDevice> device = MakeStereoNullDevice();
    AudioEngine& engine = device->GetEngine();
    ConstantGenerator generator;

    // Buffered carries no per-frame pan or Doppler, so a spatial buffered request is invalid, exactly
    // as a stereo spatial one is.
    const VoiceHandle spatial =
        engine.PlayGenerator(&generator, GeneratorVoiceParams{.Spatial = true, .Buffered = true});
    CHECK_FALSE(spatial.IsValid());
    CHECK(engine.GetActiveVoiceCount() == 0);

    const VoiceHandle ok = engine.PlayGenerator(
        &generator, GeneratorVoiceParams{.Bus = AudioBus::Master, .Buffered = true, .Gain = 1.0f});
    CHECK(ok.IsValid());
    CHECK(engine.GetActiveVoiceCount() == 1);
    engine.StopVoice(ok);
    CHECK(engine.GetActiveVoiceCount() == 0);
}

TEST_CASE("StopVoice on a buffered generator returns only once no thread can reach the generator")
{
    const Unique<AudioDevice> device = MakeStereoNullDevice();
    AudioEngine& engine = device->GetEngine();
    StereoRampGenerator generator;

    const VoiceHandle voice = engine.PlayGenerator(
        &generator, GeneratorVoiceParams{
                        .Bus = AudioBus::Master, .Channels = 2, .Buffered = true, .Gain = 1.0f});
    REQUIRE(voice.IsValid());

    // Let the fill thread render into the ring and the mixer drain some, so both threads are actively
    // referencing the voice when it is stopped.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    device->Pump(0.02f);
    CHECK(engine.GetPendingReclaimCount() == 0);

    engine.StopVoice(voice);
    // The moment StopVoice returns the borrowed generator is free to destroy: the mixer has consumed a
    // frame past the removal and the fill thread has acknowledged the Remove.
    CHECK_FALSE(engine.IsVoiceLive(voice));
    CHECK(engine.GetActiveVoiceCount() == 0);
    // The wrapper is queued for reclamation — both handshake parties are past it, but CollectDeferred
    // (run inside Pump) has not fired since the stop.
    CHECK(engine.GetPendingReclaimCount() == 1);

    // Prove the fill thread renders the generator no more: its call count cannot rise across a long
    // sleep, even while the device keeps pumping. A rise would be a use-after-free waiting to happen.
    const u64 callsAtStop = generator.RenderCalls.load(std::memory_order_relaxed);
    for (int i = 0; i < 20; ++i)
    {
        device->Pump(0.02f);
        std::this_thread::sleep_for(std::chrono::milliseconds(2));
    }
    CHECK(generator.RenderCalls.load(std::memory_order_relaxed) == callsAtStop);

    // The wrapper frees cleanly through the deferred queue once the mixer pumps again.
    for (int i = 0; i < 400 && engine.GetPendingReclaimCount() > 0; ++i)
    {
        device->Pump(1.0f / 60.0f);
    }
    CHECK(engine.GetPendingReclaimCount() == 0);
}

TEST_CASE("a buffered generator underrun is silence, never a hang or garbage")
{
    const Unique<AudioDevice> device = MakeStereoNullDevice();
    AudioEngine& engine = device->GetEngine();
    ConstantGenerator generator;

    const VoiceHandle voice = engine.PlayGenerator(
        &generator, GeneratorVoiceParams{.Bus = AudioBus::Master, .Buffered = true, .Gain = 1.0f});
    REQUIRE(voice.IsValid());

    // Pump before the fill thread is guaranteed to have filled the ring: an empty ring drains as
    // silence and returns — never blocks, never emits garbage. Whatever it produces is finite and in
    // range.
    device->Pump(0.02f);
    for (const f32 sample : device->GetDebugMixBuffer())
    {
        CHECK(std::isfinite(sample));
        CHECK(std::abs(sample) <= 1.5f);
    }

    // Once filled, the mono constant drives the master bus audibly — the buffered path reaches the mix.
    std::this_thread::sleep_for(std::chrono::milliseconds(50));
    f32 peak = 0.0f;
    for (int i = 0; i < 20; ++i)
    {
        device->Pump(0.02f);
        for (const f32 sample : device->GetDebugMixBuffer())
        {
            peak = std::max(peak, std::abs(sample));
        }
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(peak > 0.1f);

    engine.StopVoice(voice);
    CHECK_FALSE(engine.IsVoiceLive(voice));
}
