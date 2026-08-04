// The spatialization DSP veng computes itself (miniaudio's 3-D is disabled), all pure and provable
// without hardware: distance attenuation is monotone and bounded, panning is the source's direction
// in the listener's frame, Doppler has the right sign, and the occlusion factor drives the mixer's
// per-voice low-pass monotonically with zero being an exact bypass. The attenuation/pan/Doppler
// functions produce the final numbers the mixer consumes, so these pin the production math; the
// occlusion case drives the real RenderBlock mixer, since the low-pass is what a factor becomes.

#include <doctest/doctest.h>

#include <glm/gtc/quaternion.hpp>

#include <Veng/Audio/AudioBuffer.h>
#include <Veng/Audio/AudioDevice.h>
#include <Veng/Audio/AudioEngine.h>
#include <Veng/Audio/AudioSystem.h>
#include <Veng/Audio/Voice.h>

#include <cmath>
#include <numbers>
#include <vector>

using namespace Veng;
using namespace Veng::Audio;

TEST_CASE("distance attenuation is monotone and bounded")
{
    constexpr f32 minDistance = 2.0f;
    constexpr f32 maxDistance = 10.0f;

    // Full gain at and within the min distance; silence at and beyond the max.
    CHECK(DistanceAttenuation(0.0f, minDistance, maxDistance) == doctest::Approx(1.0f));
    CHECK(DistanceAttenuation(minDistance, minDistance, maxDistance) == doctest::Approx(1.0f));
    CHECK(DistanceAttenuation(maxDistance, minDistance, maxDistance) == doctest::Approx(0.0f));
    CHECK(DistanceAttenuation(50.0f, minDistance, maxDistance) == doctest::Approx(0.0f));

    // Across a fine sweep the attenuation never rises and never leaves [0, 1] — asserted as the two
    // properties, not a curve portrait.
    bool monotone = true;
    bool bounded = true;
    f32 previous = 2.0f;
    for (int i = 0; i <= 240; ++i)
    {
        const f32 distance = static_cast<f32>(i) * 0.05f;
        const f32 gain = DistanceAttenuation(distance, minDistance, maxDistance);
        bounded = bounded && gain >= 0.0f && gain <= 1.0f;
        monotone = monotone && gain <= previous + 1e-6f;
        previous = gain;
    }
    CHECK(monotone);
    CHECK(bounded);

    // A degenerate band (max <= min) is a hard cutoff at the min rather than a divide by zero.
    CHECK(DistanceAttenuation(1.0f, 5.0f, 5.0f) == doctest::Approx(1.0f));
    CHECK(DistanceAttenuation(6.0f, 5.0f, 5.0f) == doctest::Approx(0.0f));
}

TEST_CASE("panning matches the source's direction in the listener's frame")
{
    const quat facing = glm::quat(1.0f, 0.0f, 0.0f, 0.0f); // identity: -Z forward, +X right

    // Hard right pans right, hard left pans left, dead ahead is centred.
    CHECK(StereoPan(vec3(0.0f), facing, vec3(5.0f, 0.0f, 0.0f)) == doctest::Approx(1.0f));
    CHECK(StereoPan(vec3(0.0f), facing, vec3(-5.0f, 0.0f, 0.0f)) == doctest::Approx(-1.0f));
    CHECK(StereoPan(vec3(0.0f), facing, vec3(0.0f, 0.0f, -5.0f)) == doctest::Approx(0.0f));

    // A listener turned 180° about up hears world-left on its right — the case a listener-frame bug
    // passes head-on and fails turned.
    const quat turned = glm::angleAxis(std::numbers::pi_v<f32>, vec3(0.0f, 1.0f, 0.0f));
    CHECK(StereoPan(vec3(0.0f), turned, vec3(-5.0f, 0.0f, 0.0f)) == doctest::Approx(1.0f));
    CHECK(StereoPan(vec3(0.0f), turned, vec3(5.0f, 0.0f, 0.0f)) == doctest::Approx(-1.0f));

    // A source coincident with the listener is centred rather than a divide by zero.
    CHECK(StereoPan(vec3(3.0f, 3.0f, 3.0f), facing, vec3(3.0f, 3.0f, 3.0f)) ==
          doctest::Approx(0.0f));
}

TEST_CASE("Doppler has the right sign")
{
    constexpr f32 c = DefaultSpeedOfSound;
    const vec3 listener(0.0f);
    const vec3 source(10.0f, 0.0f, 0.0f);

    // A source approaching the listener raises the pitch; receding lowers it.
    CHECK(DopplerRatio(listener, vec3(0.0f), source, vec3(-5.0f, 0.0f, 0.0f), c) > 1.0f);
    CHECK(DopplerRatio(listener, vec3(0.0f), source, vec3(5.0f, 0.0f, 0.0f), c) < 1.0f);

    // A listener closing on the source raises it too — the sign a velocity-frame error flips.
    CHECK(DopplerRatio(listener, vec3(5.0f, 0.0f, 0.0f), source, vec3(0.0f), c) > 1.0f);

    // Zero radial velocity — including a purely tangential pass — leaves the pitch unchanged.
    CHECK(DopplerRatio(listener, vec3(0.0f), source, vec3(0.0f), c) == doctest::Approx(1.0f));
    CHECK(DopplerRatio(listener, vec3(0.0f), source, vec3(0.0f, 5.0f, 0.0f), c) ==
          doctest::Approx(1.0f));
}

TEST_CASE("the reverb send is bounded and rises with distance")
{
    bool monotone = true;
    bool bounded = true;
    f32 previous = -1.0f;
    for (int i = 0; i <= 120; ++i)
    {
        const f32 distance = static_cast<f32>(i) * 0.1f;
        const f32 send = ReverbSend(distance, 1.0f, 10.0f);
        bounded = bounded && send >= 0.0f && send <= 1.0f;
        monotone = monotone && send >= previous - 1e-6f;
        previous = send;
    }
    CHECK(monotone);
    CHECK(bounded);
}

namespace
{
    // Peak absolute value of the left channel after rendering one block of a max-frequency (±1
    // alternating) looping voice through the occlusion factor — the smaller the peak, the more the
    // low-pass has cut the high-frequency content. A fresh device per call so the per-slot low-pass
    // state starts clean.
    f32 OccludedPeak(const f32 occlusion)
    {
        const Unique<AudioDevice> device = AudioDevice::Create(
            AudioDeviceInfo{.Backend = AudioBackend::Null, .SampleRate = 48000, .Channels = 2});
        AudioEngine& engine = device->GetEngine();

        const std::vector<f32> alternating = {1.0f, -1.0f};
        const Ref<AudioBuffer> buffer = AudioBuffer::Create(alternating, 1, 48000);
        engine.AddVoice(buffer, VoiceParams{.Bus = AudioBus::SFX,
                                            .Gain = 1.0f,
                                            .Pan = 0.0f,
                                            .Occlusion = occlusion,
                                            .Loop = true});
        engine.Publish();

        constexpr u32 frames = 256;
        std::vector<f32> output(static_cast<usize>(frames) * 2, 0.0f);
        device->RenderBlock(output, frames);

        f32 peak = 0.0f;
        for (u32 i = 0; i < frames; ++i)
        {
            peak = std::max(peak, std::abs(output[static_cast<usize>(i) * 2]));
        }
        return peak;
    }
}

TEST_CASE("occlusion drives the low-pass monotonically, and zero is an exact bypass")
{
    const f32 open = OccludedPeak(0.0f);
    const f32 half = OccludedPeak(0.5f);
    const f32 closed = OccludedPeak(1.0f);

    // More occlusion cuts more of the alternating signal: the peak falls monotonically.
    CHECK(open > half);
    CHECK(half > closed);

    // Zero occlusion is a bypass: the centred (equal-power) ±1 signal passes through untouched, so the
    // left channel is exactly the dry alternation and a game that never sets the factor is unaffected.
    const f32 equalPower = std::cos(std::numbers::pi_v<f32> * 0.25f);
    CHECK(open == doctest::Approx(equalPower));

    const Unique<AudioDevice> device = AudioDevice::Create(
        AudioDeviceInfo{.Backend = AudioBackend::Null, .SampleRate = 48000, .Channels = 2});
    AudioEngine& engine = device->GetEngine();
    const std::vector<f32> alternating = {1.0f, -1.0f};
    const Ref<AudioBuffer> buffer = AudioBuffer::Create(alternating, 1, 48000);
    engine.AddVoice(
        buffer, VoiceParams{.Bus = AudioBus::SFX, .Gain = 1.0f, .Occlusion = 0.0f, .Loop = true});
    engine.Publish();
    std::vector<f32> output(8, 0.0f);
    device->RenderBlock(output, 4);
    CHECK(output[0] == doctest::Approx(equalPower));
    CHECK(output[2] == doctest::Approx(-equalPower));
}
