// Streaming-voice playback over a null-but-computing device: an Encoded (Vorbis) clip is decoded
// incrementally off the real-time thread and drained by the mixer exactly as a resident PCM clip is.
// Covers that a stream yields the same signal a resident clip of the same source does (so "stream"
// is "don't pre-decode", not a different sound), that a looping stream repeats seamlessly across the
// loop seam, that the decoder rides the reclamation handshake (freed only after both threads pass
// it), that the music director plays and reports a stream track, and that an empty ring is silence,
// never a hang or garbage. The known signal is the committed sine.ogg Vorbis fixture.

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Audio/AudioBuffer.h>
#include <Veng/Audio/AudioClip.h>
#include <Veng/Audio/AudioDevice.h>
#include <Veng/Audio/AudioEngine.h>

#include <algorithm>
#include <chrono>
#include <cmath>
#include <cstring>
#include <fstream>
#include <thread>
#include <vector>

using namespace Veng;
using namespace Veng::Audio;

namespace
{
    std::vector<u8> ReadFixture(const char* name)
    {
        const std::string path = std::string(VENG_COOKER_TEST_FIXTURE_DIR) + "/" + name;
        std::ifstream in(path, std::ios::binary);
        REQUIRE_MESSAGE(in.is_open(), "missing fixture: ", path);
        return std::vector<u8>((std::istreambuf_iterator<char>(in)),
                               std::istreambuf_iterator<char>());
    }

    // Fully decodes a Vorbis stream to mono float — the ahead-of-time signal a resident clip carries.
    std::vector<f32> DecodeMono(const std::vector<u8>& ogg, u32& rate, u32& channels)
    {
        const Result<Unique<VorbisMemoryDecoder>> decoder =
            VorbisMemoryDecoder::Open(std::span<const u8>(ogg));
        REQUIRE(decoder.has_value());
        rate = (*decoder)->SampleRate();
        channels = (*decoder)->Channels();
        REQUIRE(channels >= 1);

        std::vector<f32> mono;
        std::vector<f32> chunk(1024u * channels);
        while (true)
        {
            const u64 frames = (*decoder)->Read(chunk);
            if (frames == 0)
            {
                break;
            }
            for (u64 f = 0; f < frames; ++f)
            {
                f32 sum = 0.0f;
                for (u32 c = 0; c < channels; ++c)
                {
                    sum += chunk[f * channels + c];
                }
                mono.push_back(sum / static_cast<f32>(channels));
            }
        }
        return mono;
    }

    // An Encoded clip carrying the raw Vorbis bytes — the "don't pre-decode" form, built by hand as
    // the cook would (header + bitstream), so it exercises the same load path a cooked clip takes.
    AssetHandle<AudioClip> MakeEncodedClip(const std::vector<u8>& ogg, u32 rate, u32 channels,
                                           u64 frames)
    {
        const CookedAudioHeader header{
            .Version = CookedAudioVersion,
            .Storage = static_cast<u32>(CookedAudioStorage::Encoded),
            .SampleFormat = static_cast<u32>(CookedAudioSampleFormat::F32),
            .Codec = static_cast<u32>(CookedAudioCodec::Vorbis),
            .SampleRate = rate,
            .Channels = channels,
            .FrameCount = frames,
        };
        std::vector<u8> blob(sizeof(header) + ogg.size());
        std::memcpy(blob.data(), &header, sizeof(header));
        std::memcpy(blob.data() + sizeof(header), ogg.data(), ogg.size());
        const Result<Ref<AudioClip>> clip = AudioClip::Decode(blob);
        REQUIRE(clip.has_value());
        CHECK((*clip)->Storage() == AudioStorage::Encoded);
        return AssetManager::Adopt<AudioClip>(*clip);
    }

    Unique<AudioDevice> MakeMonoNullDevice(const u32 rate)
    {
        return AudioDevice::Create(
            AudioDeviceInfo{.Backend = AudioBackend::Null, .SampleRate = rate, .Channels = 1});
    }

    // Pumps the null device until it has emitted at least `wantSamples` mono samples, accumulating
    // each pump's output. A short pre-pump warm gives the decode thread time to fill the ring first,
    // so the capture starts at the clip's first sample rather than a startup underrun.
    std::vector<f32> Capture(AudioDevice& device, const usize wantSamples)
    {
        std::vector<f32> out;
        for (int i = 0; i < 400 && out.size() < wantSamples; ++i)
        {
            device.Pump(0.02f);
            const std::span<const f32> buffer = device.GetDebugMixBuffer();
            out.insert(out.end(), buffer.begin(), buffer.end());
        }
        return out;
    }

    f32 Rms(const std::span<const f32> samples)
    {
        f64 sum = 0.0;
        for (const f32 s : samples)
        {
            sum += static_cast<f64>(s) * static_cast<f64>(s);
        }
        return samples.empty()
                   ? 0.0f
                   : static_cast<f32>(std::sqrt(sum / static_cast<f64>(samples.size())));
    }
}

TEST_CASE("a stream clip plays the same signal a resident clip of its source does")
{
    const std::vector<u8> ogg = ReadFixture("sine.ogg");
    u32 rate = 0;
    u32 channels = 0;
    const std::vector<f32> mono = DecodeMono(ogg, rate, channels);
    REQUIRE(mono.size() > 1000);

    // A resident PCM clip of the fully-decoded signal, and an Encoded clip of the same bitstream.
    const Ref<AudioBuffer> buffer = AudioBuffer::Create(mono, 1, rate);
    const AssetHandle<AudioClip> resident =
        AssetManager::Adopt<AudioClip>(AudioClip::CreatePcm(buffer));
    const AssetHandle<AudioClip> streamed = MakeEncodedClip(ogg, rate, channels, mono.size());

    // Both play non-spatial on the master bus at unity; at clip-rate == device-rate the resampler is
    // an identity for either source, so the only variable is whether the decode happened ahead of
    // time or streams in — the signals must match within codec tolerance (here, exactly).
    const Unique<AudioDevice> residentDevice = MakeMonoNullDevice(rate);
    residentDevice->GetEngine().PlayOneShot(resident, OneShotParams{.Bus = AudioBus::Master});
    const std::vector<f32> residentOut = Capture(*residentDevice, mono.size());

    const Unique<AudioDevice> streamDevice = MakeMonoNullDevice(rate);
    streamDevice->GetEngine().PlayOneShot(streamed, OneShotParams{.Bus = AudioBus::Master});
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // let the ring fill before capture
    const std::vector<f32> streamOut = Capture(*streamDevice, mono.size());

    REQUIRE(residentOut.size() >= mono.size());
    REQUIRE(streamOut.size() >= mono.size());

    // Compare the signal body: the two paths reach end-of-clip through different tests (the buffer
    // voice on its frame count, the stream on an empty ring plus AtEnd), so they can retire a single
    // sample apart at the very end. Excluding that sub-millisecond boundary, the streamed signal is
    // bit-for-bit the resident one — "stream" is "don't pre-decode", not a different sound.
    const usize compareCount = mono.size() - 8;
    f32 maxDiff = 0.0f;
    f32 peak = 0.0f;
    for (usize i = 0; i < compareCount; ++i)
    {
        maxDiff = std::max(maxDiff, std::abs(streamOut[i] - residentOut[i]));
        peak = std::max(peak, std::abs(residentOut[i]));
    }
    CHECK(peak > 0.05f);      // the signal is real, not silence
    CHECK(maxDiff < 1.0e-4f); // streaming reproduces the resident signal exactly
}

TEST_CASE("a looping stream repeats seamlessly across the loop seam")
{
    const std::vector<u8> ogg = ReadFixture("sine.ogg");
    u32 rate = 0;
    u32 channels = 0;
    const std::vector<f32> mono = DecodeMono(ogg, rate, channels);
    const usize period = mono.size();
    REQUIRE(period > 1000);

    const Unique<AudioDevice> device = MakeMonoNullDevice(rate);
    AudioEngine& engine = device->GetEngine();
    const AssetHandle<AudioClip> streamed = MakeEncodedClip(ogg, rate, channels, period);

    const VoiceHandle voice =
        engine.PlayOneShot(streamed, OneShotParams{.Bus = AudioBus::Master, .Loop = true});
    REQUIRE(voice.IsValid());
    std::this_thread::sleep_for(std::chrono::milliseconds(100)); // ring holds several loop copies
    const std::vector<f32> out = Capture(*device, period * 2);
    REQUIRE(out.size() >= period * 2);

    // The second loop iteration is sample-for-sample the first: a gap or overlap at the seam would
    // shift or drop samples and break the alignment. Assert the aggregate over a whole period, and
    // that the loop is live and non-trivial.
    f32 maxDiff = 0.0f;
    for (usize i = 0; i < period; ++i)
    {
        maxDiff = std::max(maxDiff, std::abs(out[period + i] - out[i]));
    }
    const f32 firstRms = Rms(std::span<const f32>(out.data(), period));
    CHECK(firstRms > 0.02f);
    CHECK(maxDiff < 2.0e-3f);
    CHECK(engine.IsVoiceLive(voice)); // a loop never retires by exhaustion
}

TEST_CASE("a stream voice's decoder is freed only after the reclamation handshake")
{
    const std::vector<u8> ogg = ReadFixture("sine.ogg");
    u32 rate = 0;
    u32 channels = 0;
    const std::vector<f32> mono = DecodeMono(ogg, rate, channels);

    const Unique<AudioDevice> device = MakeMonoNullDevice(rate);
    AudioEngine& engine = device->GetEngine();
    const AssetHandle<AudioClip> streamed = MakeEncodedClip(ogg, rate, channels, mono.size());

    const VoiceHandle voice =
        engine.PlayOneShot(streamed, OneShotParams{.Bus = AudioBus::Master, .Loop = true});
    REQUIRE(voice.IsValid());
    CHECK(engine.GetPendingReclaimCount() == 0);

    engine.StopVoice(voice);
    CHECK_FALSE(engine.IsVoiceLive(voice));
    CHECK(engine.GetActiveVoiceCount() == 0);
    // The stream is queued for reclamation, not yet freed: the mixer must pass the last frame that
    // referenced its ring and the decode thread must release it before the decoder can be destroyed.
    CHECK(engine.GetPendingReclaimCount() == 1);

    for (int i = 0; i < 400 && engine.GetPendingReclaimCount() > 0; ++i)
    {
        device->Pump(1.0f / 60.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK(engine.GetPendingReclaimCount() == 0);
}

TEST_CASE("the music director plays a stream track and Current reports it")
{
    const std::vector<u8> ogg = ReadFixture("sine.ogg");
    u32 rate = 0;
    u32 channels = 0;
    const std::vector<f32> mono = DecodeMono(ogg, rate, channels);

    const Unique<AudioDevice> device = MakeMonoNullDevice(rate);
    AudioEngine& engine = device->GetEngine();
    MusicDirector& music = engine.Music();
    const AssetHandle<AudioClip> track = MakeEncodedClip(ogg, rate, channels, mono.size());

    music.Set(track, MusicTransition{.FadeSeconds = 0.0f, .Loop = true});
    REQUIRE(music.GetVoiceCount() == 1);
    CHECK(music.Current().Get() == track.Get());

    std::this_thread::sleep_for(std::chrono::milliseconds(100));
    f32 peak = 0.0f;
    for (int i = 0; i < 20; ++i)
    {
        device->Pump(0.02f);
        for (const f32 sample : device->GetDebugMixBuffer())
        {
            peak = std::max(peak, std::abs(sample));
        }
    }
    CHECK(peak > 0.02f);               // the stream music actually sounds
    CHECK(music.GetVoiceCount() == 1); // the looping track stays the one logical track
}

TEST_CASE("a stream underrun is silence, never a hang or garbage")
{
    const std::vector<u8> ogg = ReadFixture("sine.ogg");
    u32 rate = 0;
    u32 channels = 0;
    const std::vector<f32> mono = DecodeMono(ogg, rate, channels);

    const Unique<AudioDevice> device = MakeMonoNullDevice(rate);
    AudioEngine& engine = device->GetEngine();
    const AssetHandle<AudioClip> streamed = MakeEncodedClip(ogg, rate, channels, mono.size());

    const VoiceHandle voice = engine.PlayOneShot(streamed, OneShotParams{.Bus = AudioBus::Master});
    REQUIRE(voice.IsValid());

    // Pump before the decode thread can fill the ring: an empty ring outputs silence and returns —
    // never blocks, never emits garbage. Whatever it produces is finite and in range.
    device->Pump(0.02f);
    for (const f32 sample : device->GetDebugMixBuffer())
    {
        CHECK(std::isfinite(sample));
        CHECK(std::abs(sample) <= 1.5f);
    }

    // Let it play fully out and retire; a drained, retired stream is exact silence.
    for (int i = 0; i < 400 && engine.IsVoiceLive(voice); ++i)
    {
        device->Pump(1.0f / 60.0f);
        std::this_thread::sleep_for(std::chrono::milliseconds(1));
    }
    CHECK_FALSE(engine.IsVoiceLive(voice));

    device->Pump(0.05f);
    device->Pump(0.05f);
    f32 drainedPeak = 0.0f;
    for (const f32 sample : device->GetDebugMixBuffer())
    {
        drainedPeak = std::max(drainedPeak, std::abs(sample));
    }
    CHECK(drainedPeak < 1.0e-4f);
}
