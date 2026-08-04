// Audio-clip cook->load round trip: cooks a generated sine WAV and a committed Ogg fixture through
// the AudioImporter and loads the results with Audio::AudioClip::Decode. Covers the sample-mode PCM
// round trip (rate/channels/frame-count exact, samples within an amplitude epsilon), stream-mode
// (an encoded blob materially smaller than the decoded one, whose incremental decoder yields the
// same signal), the version-mismatch reject, and the located cook errors (a missing source, a
// stream mode on a non-.ogg source, a zero-length clip).

#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <numbers>
#include <random>

#include "support/TempPath.h"

#include <doctest/doctest.h>
#include <fmt/format.h>

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Audio/AudioBuffer.h>
#include <Veng/Audio/AudioClip.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>

using namespace Veng;
using namespace Veng::Cook;

namespace
{
    constexpr u32 WavSampleRate = 48000;
    constexpr u32 WavFrames = 4800;
    constexpr f32 WavFrequency = 440.0f;
    constexpr f32 WavAmplitude = 0.5f;

    // The i16 sample a WAV frame carries, from the source sine.
    i16 SineSampleI16(const u32 frame)
    {
        const f64 phase = 2.0 * std::numbers::pi * WavFrequency * frame / WavSampleRate;
        return static_cast<i16>(std::lround(std::sin(phase) * WavAmplitude * 32767.0));
    }

    void AppendU16(vector<u8>& out, const u16 value)
    {
        out.push_back(static_cast<u8>(value & 0xFF));
        out.push_back(static_cast<u8>((value >> 8) & 0xFF));
    }

    void AppendU32(vector<u8>& out, const u32 value)
    {
        for (int shift = 0; shift < 32; shift += 8)
        {
            out.push_back(static_cast<u8>((value >> shift) & 0xFF));
        }
    }

    void AppendTag(vector<u8>& out, const char (&tag)[5])
    {
        for (int i = 0; i < 4; ++i)
        {
            out.push_back(static_cast<u8>(tag[i]));
        }
    }

    // A 16-bit mono PCM WAV carrying `frames` samples of the source sine (0 frames = a valid but
    // empty clip, the zero-length error case).
    vector<u8> MakeSineWav(const u32 frames)
    {
        const u32 dataBytes = frames * sizeof(i16);
        vector<u8> wav;
        AppendTag(wav, "RIFF");
        AppendU32(wav, 36 + dataBytes);
        AppendTag(wav, "WAVE");
        AppendTag(wav, "fmt ");
        AppendU32(wav, 16);
        AppendU16(wav, 1); // PCM
        AppendU16(wav, 1); // mono
        AppendU32(wav, WavSampleRate);
        AppendU32(wav, WavSampleRate * sizeof(i16)); // byte rate (mono)
        AppendU16(wav, sizeof(i16));                 // block align
        AppendU16(wav, 16);                          // bits per sample
        AppendTag(wav, "data");
        AppendU32(wav, dataBytes);
        for (u32 i = 0; i < frames; ++i)
        {
            const i16 sample = SineSampleI16(i);
            wav.push_back(static_cast<u8>(sample & 0xFF));
            wav.push_back(static_cast<u8>((sample >> 8) & 0xFF));
        }
        return wav;
    }

    void WriteBytes(const path& file, const std::span<const u8> bytes)
    {
        std::ofstream out(file, std::ios::binary);
        REQUIRE(out.is_open());
        out.write(reinterpret_cast<const char*>(bytes.data()),
                  static_cast<std::streamsize>(bytes.size()));
    }

    void WriteText(const path& file, const string& text)
    {
        std::ofstream out(file, std::ios::binary);
        REQUIRE(out.is_open());
        out.write(text.data(), static_cast<std::streamsize>(text.size()));
    }

    // A unique scratch directory for a case; ctest may run the cooker suite alongside others.
    path ScratchDir()
    {
        std::random_device rng;
        const path dir = Veng::TestSupport::TempDir() / fmt::format("veng_audio_{:08x}", rng());
        std::filesystem::create_directories(dir);
        return dir;
    }

    // Cooks a one-entry pack the caller has populated with sources and returns the cooked blob.
    Result<vector<u8>> CookOne(const path& dir, const string& type, const string& source,
                               const AssetId id)
    {
        const string pack = fmt::format(
            R"({{ "version": 1, "assets": [ {{ "id": "0x{:016X}", "type": "{}", "source": "{}" }} ] }})",
            id.Value, type, source);
        WriteText(dir / "pack.json", pack);

        Cooker cooker;
        RegisterBuiltinImporters(cooker);
        const path out = dir / "out.vengpack";
        const VoidResult cooked = cooker.CookPack(dir / "pack.json", out);
        if (!cooked)
        {
            return std::unexpected(cooked.error());
        }
        const Result<ArchiveReader> reader = ArchiveReader::Open(out);
        if (!reader)
        {
            return std::unexpected(reader.error());
        }
        const optional<ArchiveEntry> entry = reader->Find(id);
        if (!entry)
        {
            return std::unexpected(string("cooked entry not found"));
        }
        return vector<u8>(entry->Blob.begin(), entry->Blob.end());
    }
}

TEST_CASE("Cooker: a sample-mode WAV round-trips through cook and load")
{
    const path dir = ScratchDir();
    WriteBytes(dir / "sine.wav", MakeSineWav(WavFrames));
    WriteText(dir / "sine.audio.json", R"({ "source": "sine.wav", "mode": "sample" })");

    const Result<vector<u8>> blob = CookOne(dir, "AudioClip", "sine.audio.json", AssetId{0xA1D01});
    REQUIRE(blob.has_value());

    CookedAudioHeader header{};
    std::memcpy(&header, blob->data(), sizeof(header));
    CHECK(header.Version == CookedAudioVersion);
    CHECK(header.Storage == static_cast<u32>(CookedAudioStorage::Pcm));
    CHECK(header.SampleFormat == static_cast<u32>(CookedAudioSampleFormat::I16));

    const Result<Ref<Audio::AudioClip>> clip = Audio::AudioClip::Decode(*blob);
    REQUIRE(clip.has_value());
    const Audio::AudioClip& audio = **clip;

    CHECK(audio.Storage() == Audio::AudioStorage::Pcm);
    CHECK(audio.SampleRate() == WavSampleRate);
    CHECK(audio.Channels() == 1);
    CHECK(audio.FrameCount() == WavFrames);

    REQUIRE(audio.Buffer() != nullptr);
    const std::span<const f32> samples = audio.Buffer()->Samples();
    REQUIRE(samples.size() == WavFrames);
    // The loaded float samples reproduce the source sine within the 16-bit quantization step.
    f32 maxError = 0.0f;
    for (u32 i = 0; i < WavFrames; ++i)
    {
        const f32 expected = static_cast<f32>(SineSampleI16(i)) / 32768.0f;
        maxError = std::max(maxError, std::abs(samples[i] - expected));
    }
    CHECK(maxError < 1.0e-4f);
}

TEST_CASE("Cooker: a stream-mode clip stores encoded and decodes to the same signal")
{
    const path fixtureOgg = path(VENG_COOKER_TEST_FIXTURE_DIR) / "sine.ogg";

    const path sampleDir = ScratchDir();
    std::filesystem::copy_file(fixtureOgg, sampleDir / "sine.ogg");
    WriteText(sampleDir / "sine.audio.json", R"({ "source": "sine.ogg", "mode": "sample" })");
    const Result<vector<u8>> sampleBlob =
        CookOne(sampleDir, "AudioClip", "sine.audio.json", AssetId{0xA1D02});
    REQUIRE(sampleBlob.has_value());

    const path streamDir = ScratchDir();
    std::filesystem::copy_file(fixtureOgg, streamDir / "sine.ogg");
    WriteText(streamDir / "sine.audio.json", R"({ "source": "sine.ogg", "mode": "stream" })");
    const Result<vector<u8>> streamBlob =
        CookOne(streamDir, "AudioClip", "sine.audio.json", AssetId{0xA1D03});
    REQUIRE(streamBlob.has_value());

    // "stream" is "don't pre-decode": the encoded blob is far smaller than the decoded PCM one.
    CHECK(streamBlob->size() * 2 < sampleBlob->size());

    const Result<Ref<Audio::AudioClip>> samplePcm = Audio::AudioClip::Decode(*sampleBlob);
    REQUIRE(samplePcm.has_value());
    CHECK((*samplePcm)->Storage() == Audio::AudioStorage::Pcm);
    REQUIRE((*samplePcm)->Buffer() != nullptr);
    const std::span<const f32> decodedAtCook = (*samplePcm)->Buffer()->Samples();

    const Result<Ref<Audio::AudioClip>> streamed = Audio::AudioClip::Decode(*streamBlob);
    REQUIRE(streamed.has_value());
    CHECK((*streamed)->Storage() == Audio::AudioStorage::Encoded);
    CHECK((*streamed)->Codec() == Audio::AudioCodec::Vorbis);

    const Result<Veng::Unique<Audio::VorbisMemoryDecoder>> decoder = (*streamed)->OpenDecoder();
    REQUIRE(decoder.has_value());
    const u32 channels = (*decoder)->Channels();
    REQUIRE(channels >= 1);

    vector<f32> decodedAtPlay;
    vector<f32> chunk(1024u * channels);
    while (true)
    {
        const u64 frames = (*decoder)->Read(chunk);
        if (frames == 0)
        {
            break;
        }
        decodedAtPlay.insert(decodedAtPlay.end(), chunk.begin(),
                             chunk.begin() + static_cast<std::ptrdiff_t>(frames * channels));
    }

    // Both paths run the same Vorbis decoder over the same bitstream, so the incremental play-time
    // decode yields the same samples the cook decoded ahead of time.
    REQUIRE(decodedAtPlay.size() == decodedAtCook.size());
    f32 maxError = 0.0f;
    for (usize i = 0; i < decodedAtPlay.size(); ++i)
    {
        maxError = std::max(maxError, std::abs(decodedAtPlay[i] - decodedAtCook[i]));
    }
    CHECK(maxError < 1.0e-5f);
}

TEST_CASE("Audio clip load rejects a version mismatch")
{
    CookedAudioHeader header{
        .Version = CookedAudioVersion + 1,
        .Storage = static_cast<u32>(CookedAudioStorage::Pcm),
        .SampleFormat = static_cast<u32>(CookedAudioSampleFormat::F32),
        .Codec = static_cast<u32>(CookedAudioCodec::None),
        .SampleRate = 48000,
        .Channels = 1,
        .FrameCount = 1,
    };
    vector<u8> blob(sizeof(header) + sizeof(f32), 0);
    std::memcpy(blob.data(), &header, sizeof(header));

    const Result<Ref<Audio::AudioClip>> clip = Audio::AudioClip::Decode(blob);
    REQUIRE_FALSE(clip.has_value());
    CHECK(clip.error().find("version") != string::npos);
}

TEST_CASE("Cooker: a missing audio source is a located cook error")
{
    const path dir = ScratchDir();
    WriteText(dir / "sine.audio.json", R"({ "source": "absent.wav", "mode": "sample" })");
    const Result<vector<u8>> blob = CookOne(dir, "AudioClip", "sine.audio.json", AssetId{0xA1D04});
    REQUIRE_FALSE(blob.has_value());
    CHECK(blob.error().find("sine.audio.json") != string::npos);
}

TEST_CASE("Cooker: stream mode on a non-ogg source is a located cook error")
{
    const path dir = ScratchDir();
    WriteBytes(dir / "sine.wav", MakeSineWav(WavFrames));
    WriteText(dir / "sine.audio.json", R"({ "source": "sine.wav", "mode": "stream" })");
    const Result<vector<u8>> blob = CookOne(dir, "AudioClip", "sine.audio.json", AssetId{0xA1D05});
    REQUIRE_FALSE(blob.has_value());
    CHECK(blob.error().find(".ogg") != string::npos);
    CHECK(blob.error().find("sine.audio.json") != string::npos);
}

TEST_CASE("Cooker: a zero-length clip is a located cook error")
{
    const path dir = ScratchDir();
    WriteBytes(dir / "silent.wav", MakeSineWav(0));
    WriteText(dir / "silent.audio.json", R"({ "source": "silent.wav", "mode": "sample" })");
    const Result<vector<u8>> blob =
        CookOne(dir, "AudioClip", "silent.audio.json", AssetId{0xA1D06});
    REQUIRE_FALSE(blob.has_value());
    CHECK(blob.error().find("empty clip") != string::npos);
    CHECK(blob.error().find("silent.audio.json") != string::npos);
}
