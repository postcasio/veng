// A non-spatial generator voice may render stereo, mixed as an interleaved pair past the mono pan
// stage — all provable on the null device, which runs the whole mix path on the main thread. The
// properties: a stereo generator's two channels stay independent (a mono-then-pan path could not
// hold them), the stereo branch bypasses EqualPowerPan so a Pan value does not move its image, a
// mono generator still pans exactly as before, a spatial stereo request is rejected, and a stereo
// voice's reverb send is the mono channel average (L + R) * 0.5.

#include <doctest/doctest.h>

#include <Veng/Audio/AudioBus.h>
#include <Veng/Audio/AudioDevice.h>
#include <Veng/Audio/AudioEngine.h>
#include <Veng/Audio/AudioGenerator.h>
#include <Veng/Audio/Voice.h>

#include <algorithm>
#include <cmath>
#include <span>
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

    // A generator writing a constant authored L/R image. Contract-correct at either width: it writes
    // frames × channels (a mono drive collapses to the left value), so it exercises the stereo path
    // when registered with Channels == 2.
    struct StereoConstantGenerator final : IAudioGenerator
    {
        f32 Left = 0.0f;
        f32 Right = 0.0f;
        void Render(f32* out, const u32 frames, const u32 channels, u32 /*sampleRate*/) override
        {
            for (u32 f = 0; f < frames; ++f)
            {
                out[f * channels] = Left;
                for (u32 c = 1; c < channels; ++c)
                {
                    out[f * channels + c] = Right;
                }
            }
        }
    };

    // A mono generator emitting a constant (the default width), for the mono-then-pan path.
    struct MonoConstantGenerator final : IAudioGenerator
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

    // Renders one block of a stereo generator (left, right) on the Master bus with the given reverb
    // send and full wet, returning the interleaved stereo output. A fresh device means a fresh
    // reverb tail, so two returned buffers differ only by their send.
    std::vector<f32> RenderStereoWithReverb(f32 left, f32 right, f32 send, u32 frames)
    {
        StereoConstantGenerator gen;
        gen.Left = left;
        gen.Right = right;
        const Unique<AudioDevice> device = MakeNullDevice();
        AudioEngine& engine = device->GetEngine();
        engine.SetReverbParams(ReverbParams{.RoomSize = 0.8f, .Wet = 1.0f});

        const VoiceHandle voice = engine.PlayGenerator(
            &gen, GeneratorVoiceParams{.Bus = AudioBus::Master, .Spatial = false, .Channels = 2});
        REQUIRE(voice.IsValid());
        engine.SetVoiceParams(
            voice, VoiceParams{.Bus = AudioBus::Master, .Gain = 1.0f, .ReverbSend = send});
        engine.Publish();

        std::vector<f32> out(static_cast<usize>(frames) * device->GetChannels(), 0.0f);
        device->RenderBlock(out, frames);
        engine.StopVoice(voice);
        return out;
    }
}

TEST_CASE("a stereo generator keeps its channels independent through the mix")
{
    StereoConstantGenerator gen;
    gen.Left = 0.5f;
    gen.Right = -0.5f;
    const Unique<AudioDevice> device = MakeNullDevice();
    AudioEngine& engine = device->GetEngine();

    const VoiceHandle voice = engine.PlayGenerator(
        &gen, GeneratorVoiceParams{.Bus = AudioBus::Master, .Spatial = false, .Channels = 2});
    REQUIRE(voice.IsValid());

    engine.Publish();
    constexpr u32 frames = 128;
    std::vector<f32> out(static_cast<usize>(frames) * device->GetChannels(), 0.0f);
    device->RenderBlock(out, frames);

    // Left carries +0.5, right carries -0.5: a mono-then-pan path could hold only one summed stream,
    // so distinct per-channel content surviving is the property this asserts.
    CHECK(out[0] == doctest::Approx(0.5f).epsilon(0.01));
    CHECK(out[1] == doctest::Approx(-0.5f).epsilon(0.01));
    CHECK(out[static_cast<usize>(frames - 1) * 2] == doctest::Approx(0.5f).epsilon(0.01));
    CHECK(out[static_cast<usize>(frames - 1) * 2 + 1] == doctest::Approx(-0.5f).epsilon(0.01));

    engine.StopVoice(voice);
}

TEST_CASE("a stereo generator bypasses the pan stage")
{
    StereoConstantGenerator gen;
    gen.Left = 0.5f;
    gen.Right = 0.5f;
    const Unique<AudioDevice> device = MakeNullDevice();
    AudioEngine& engine = device->GetEngine();

    const VoiceHandle voice = engine.PlayGenerator(
        &gen, GeneratorVoiceParams{.Bus = AudioBus::Master, .Spatial = false, .Channels = 2});
    REQUIRE(voice.IsValid());

    // Hard-pan left on the snapshot: a mono voice would collapse entirely to the left channel. The
    // stereo branch never calls EqualPowerPan, so both channels survive unmoved.
    engine.SetVoiceParams(voice, VoiceParams{.Bus = AudioBus::Master, .Gain = 1.0f, .Pan = -1.0f});
    engine.Publish();
    constexpr u32 frames = 128;
    std::vector<f32> out(static_cast<usize>(frames) * device->GetChannels(), 0.0f);
    device->RenderBlock(out, frames);

    CHECK(out[0] == doctest::Approx(0.5f).epsilon(0.01));
    CHECK(out[1] == doctest::Approx(0.5f).epsilon(0.01)); // the pan did not silence the right

    engine.StopVoice(voice);
}

TEST_CASE("a mono generator still renders through the pan stage unchanged")
{
    MonoConstantGenerator gen;
    gen.Value = 0.5f;
    const Unique<AudioDevice> device = MakeNullDevice();
    AudioEngine& engine = device->GetEngine();

    // Default width (mono). Hard-pan left: equal-power pan puts the whole signal in the left channel
    // and silence in the right — the mono-then-pan behaviour the additive stereo branch leaves be.
    const VoiceHandle voice =
        engine.PlayGenerator(&gen, GeneratorVoiceParams{.Bus = AudioBus::Master, .Spatial = false});
    REQUIRE(voice.IsValid());
    engine.SetVoiceParams(voice, VoiceParams{.Bus = AudioBus::Master, .Gain = 1.0f, .Pan = -1.0f});
    engine.Publish();
    constexpr u32 frames = 128;
    std::vector<f32> out(static_cast<usize>(frames) * device->GetChannels(), 0.0f);
    device->RenderBlock(out, frames);

    CHECK(out[0] == doctest::Approx(0.5f).epsilon(0.01)); // full signal in the left
    CHECK(out[1] == doctest::Approx(0.0f).epsilon(0.01)); // silent right

    engine.StopVoice(voice);
}

TEST_CASE("a spatial stereo generator request is rejected")
{
    StereoConstantGenerator gen; // never driven — the request is refused before it registers
    const Unique<AudioDevice> device = MakeNullDevice();
    AudioEngine& engine = device->GetEngine();

    const VoiceHandle voice = engine.PlayGenerator(
        &gen, GeneratorVoiceParams{.Spatial = true, .Channels = 2, .MaxDistance = 100.0f});
    CHECK_FALSE(voice.IsValid());
    CHECK(engine.GetActiveVoiceCount() == 0);
}

TEST_CASE("a stereo voice folds its reverb send to the channel average")
{
    constexpr u32 frames = 8192;

    // Opposite channels average to zero, so the reverb receives nothing and the send render matches
    // the no-send render exactly — the fold is a true (L + R) average, not |L| + |R| or one channel.
    const std::vector<f32> cancelSend = RenderStereoWithReverb(0.5f, -0.5f, 1.0f, frames);
    const std::vector<f32> cancelDry = RenderStereoWithReverb(0.5f, -0.5f, 0.0f, frames);
    f32 cancelDiff = 0.0f;
    for (usize i = 0; i < cancelSend.size(); ++i)
    {
        cancelDiff = std::max(cancelDiff, std::abs(cancelSend[i] - cancelDry[i]));
    }
    CHECK(cancelDiff < 1e-4f);

    // Equal channels average to 0.5, so the reverb is driven and a wet tail appears over the dry.
    const std::vector<f32> sameSend = RenderStereoWithReverb(0.5f, 0.5f, 1.0f, frames);
    const std::vector<f32> sameDry = RenderStereoWithReverb(0.5f, 0.5f, 0.0f, frames);
    f32 sameDiff = 0.0f;
    for (usize i = 0; i < sameSend.size(); ++i)
    {
        sameDiff = std::max(sameDiff, std::abs(sameSend[i] - sameDry[i]));
    }
    CHECK(sameDiff > 0.01f);
}
