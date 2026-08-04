#include "AudioImporter.h"
#include <Veng/Asset/Path.h>

#include <algorithm>
#include <cstdint>
#include <cstring>
#include <fstream>

#include <fmt/format.h>

#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Cook/JsonFile.h>

#include <dr_wav.h>

// stb_vorbis is compiled once in src/Vendor/StbVorbis.cpp; here we take declarations only. The
// config macros match that TU so the declared and defined surfaces agree.
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

namespace Veng::Cook
{
    namespace
    {
        /// @brief One decoded PCM signal: interleaved samples plus its shape.
        struct DecodedPcm
        {
            vector<u8> Payload;
            CookedAudioSampleFormat SampleFormat = CookedAudioSampleFormat::F32;
            u32 Channels = 0;
            u32 SampleRate = 0;
            u64 FrameCount = 0;
        };

        /// @brief Reads a whole binary file into memory.
        Result<vector<u8>> ReadBinaryFile(const path& file, const string& what)
        {
            std::ifstream in(file, std::ios::binary);
            if (!in)
            {
                return std::unexpected(fmt::format("{}: failed to open '{}'", what, file.string()));
            }
            in.seekg(0, std::ios::end);
            const std::streamoff size = in.tellg();
            if (size < 0)
            {
                return std::unexpected(fmt::format("{}: failed to size '{}'", what, file.string()));
            }
            in.seekg(0, std::ios::beg);
            vector<u8> bytes(static_cast<usize>(size));
            in.read(reinterpret_cast<char*>(bytes.data()), size);
            if (!in)
            {
                return std::unexpected(fmt::format("{}: failed reading '{}'", what, file.string()));
            }
            return bytes;
        }

        /// @brief Lowercases a file extension for a case-insensitive source-format check.
        string LowerExtension(const path& file)
        {
            string ext = file.extension().string();
            std::ranges::transform(ext, ext.begin(), [](unsigned char c)
                                   { return static_cast<char>(std::tolower(c)); });
            return ext;
        }

        /// @brief Decodes a WAV to interleaved 16-bit PCM through dr_wav.
        Result<DecodedPcm> DecodeWav(const std::span<const u8> bytes, const string& what)
        {
            unsigned int channels = 0;
            unsigned int sampleRate = 0;
            drwav_uint64 frameCount = 0;
            drwav_int16* pcm = drwav_open_memory_and_read_pcm_frames_s16(
                bytes.data(), bytes.size(), &channels, &sampleRate, &frameCount, nullptr);
            if (pcm == nullptr)
            {
                return std::unexpected(
                    fmt::format("{}: dr_wav could not decode the WAV source", what));
            }

            DecodedPcm decoded;
            decoded.SampleFormat = CookedAudioSampleFormat::I16;
            decoded.Channels = channels;
            decoded.SampleRate = sampleRate;
            decoded.FrameCount = frameCount;
            const usize sampleCount = static_cast<usize>(frameCount) * channels;
            decoded.Payload.resize(sampleCount * sizeof(std::int16_t));
            std::memcpy(decoded.Payload.data(), pcm, decoded.Payload.size());
            drwav_free(pcm, nullptr);
            return decoded;
        }

        /// @brief Decodes an Ogg Vorbis stream to interleaved float PCM through stb_vorbis.
        Result<DecodedPcm> DecodeOgg(const std::span<const u8> bytes, const string& what)
        {
            int error = 0;
            stb_vorbis* v = stb_vorbis_open_memory(bytes.data(), static_cast<int>(bytes.size()),
                                                   &error, nullptr);
            if (v == nullptr)
            {
                return std::unexpected(fmt::format(
                    "{}: stb_vorbis could not open the Ogg source (error {})", what, error));
            }
            const stb_vorbis_info info = stb_vorbis_get_info(v);
            const unsigned int frames = stb_vorbis_stream_length_in_samples(v);

            vector<f32> samples(static_cast<usize>(frames) * info.channels);
            usize total = 0;
            while (true)
            {
                const int remaining = static_cast<int>(samples.size() - total);
                if (remaining <= 0)
                {
                    break;
                }
                const int got = stb_vorbis_get_samples_float_interleaved(
                    v, info.channels, samples.data() + total, remaining);
                if (got <= 0)
                {
                    break;
                }
                total += static_cast<usize>(got) * info.channels;
            }
            stb_vorbis_close(v);

            DecodedPcm decoded;
            decoded.SampleFormat = CookedAudioSampleFormat::F32;
            decoded.Channels = static_cast<u32>(info.channels);
            decoded.SampleRate = info.sample_rate;
            decoded.FrameCount = total / info.channels;
            decoded.Payload.resize(total * sizeof(f32));
            std::memcpy(decoded.Payload.data(), samples.data(), decoded.Payload.size());
            return decoded;
        }

        /// @brief Reads an Ogg Vorbis stream's shape without decoding its samples.
        Result<DecodedPcm> ProbeOgg(const std::span<const u8> bytes, const string& what)
        {
            int error = 0;
            stb_vorbis* v = stb_vorbis_open_memory(bytes.data(), static_cast<int>(bytes.size()),
                                                   &error, nullptr);
            if (v == nullptr)
            {
                return std::unexpected(fmt::format(
                    "{}: stb_vorbis could not open the Ogg source (error {})", what, error));
            }
            const stb_vorbis_info info = stb_vorbis_get_info(v);
            const unsigned int frames = stb_vorbis_stream_length_in_samples(v);
            stb_vorbis_close(v);

            DecodedPcm shape;
            shape.SampleFormat = CookedAudioSampleFormat::F32;
            shape.Channels = static_cast<u32>(info.channels);
            shape.SampleRate = info.sample_rate;
            shape.FrameCount = frames;
            return shape;
        }

        /// @brief Packs a header and payload into the cooked blob's byte layout.
        vector<u8> PackBlob(const CookedAudioHeader& header, const std::span<const u8> payload)
        {
            vector<u8> blob(sizeof(header) + payload.size());
            std::memcpy(blob.data(), &header, sizeof(header));
            if (!payload.empty())
            {
                std::memcpy(blob.data() + sizeof(header), payload.data(), payload.size());
            }
            return blob;
        }
    }

    AssetTypeId AudioImporter::Type() const
    {
        return AssetTypes::AudioClip;
    }

    ImporterConcurrency AudioImporter::Concurrency() const
    {
        return ImporterConcurrency::Serialized;
    }

    Result<vector<u8>> AudioImporter::Cook(const CookContext& context, const json& entry) const
    {
        if (!entry.contains("source") || !entry["source"].is_string())
        {
            return std::unexpected(string("audio importer: missing or invalid 'source'"));
        }

        const path sourcePath = context.PackDir / entry["source"].get<string>();
        const Result<json> sourceJsonResult = ReadJsonFile(sourcePath, "audio importer");
        if (!sourceJsonResult)
        {
            return std::unexpected(sourceJsonResult.error());
        }
        const json& audioJson = *sourceJsonResult;

        if (!audioJson.contains("source") || !audioJson["source"].is_string())
        {
            return std::unexpected(fmt::format("audio importer: '{}': missing or invalid 'source'",
                                               sourcePath.string()));
        }
        // "mode" defaults to "sample": the short-effect form is the common case.
        const string mode = audioJson.contains("mode") && audioJson["mode"].is_string()
                                ? audioJson["mode"].get<string>()
                                : string("sample");
        if (mode != "sample" && mode != "stream")
        {
            return std::unexpected(fmt::format(
                "audio importer: '{}': unknown mode '{}' (expected \"sample\" or \"stream\")",
                sourcePath.string(), mode));
        }

        const path audioPath = sourcePath.parent_path() / audioJson["source"].get<string>();
        context.RecordDependency(audioPath);
        const string ext = LowerExtension(audioPath);

        const string what = fmt::format("audio importer: '{}'", sourcePath.string());
        const Result<vector<u8>> bytesResult = ReadBinaryFile(audioPath, what);
        if (!bytesResult)
        {
            return std::unexpected(bytesResult.error());
        }
        const vector<u8>& bytes = *bytesResult;

        if (mode == "stream")
        {
            // No PCM->Vorbis encoder is vendored, so a stream source is not transcoded: it must
            // already be Ogg Vorbis. A WAV in stream mode is a located error, not a silent decode.
            if (ext != ".ogg")
            {
                return std::unexpected(fmt::format(
                    "audio importer: '{}': stream mode requires an .ogg source (no Vorbis encoder "
                    "is vendored to transcode '{}')",
                    sourcePath.string(), audioPath.string()));
            }
            const Result<DecodedPcm> shape = ProbeOgg(bytes, what);
            if (!shape)
            {
                return std::unexpected(shape.error());
            }
            if (shape->FrameCount == 0 || shape->Channels == 0)
            {
                return std::unexpected(
                    fmt::format("audio importer: '{}': '{}' decodes to an empty clip",
                                sourcePath.string(), audioPath.string()));
            }
            const CookedAudioHeader header{
                .Version = CookedAudioVersion,
                .Storage = static_cast<u32>(CookedAudioStorage::Encoded),
                .SampleFormat = static_cast<u32>(CookedAudioSampleFormat::F32),
                .Codec = static_cast<u32>(CookedAudioCodec::Vorbis),
                .SampleRate = shape->SampleRate,
                .Channels = shape->Channels,
                .FrameCount = shape->FrameCount,
            };
            return PackBlob(header, bytes);
        }

        Result<DecodedPcm> decoded = std::unexpected(string("unset"));
        if (ext == ".wav")
        {
            decoded = DecodeWav(bytes, what);
        }
        else if (ext == ".ogg")
        {
            decoded = DecodeOgg(bytes, what);
        }
        else
        {
            return std::unexpected(fmt::format(
                "audio importer: '{}': unsupported source extension '{}' (expected .wav or .ogg)",
                sourcePath.string(), ext));
        }
        if (!decoded)
        {
            return std::unexpected(decoded.error());
        }
        if (decoded->FrameCount == 0 || decoded->Channels == 0)
        {
            return std::unexpected(
                fmt::format("audio importer: '{}': '{}' decodes to an empty clip",
                            sourcePath.string(), audioPath.string()));
        }

        const CookedAudioHeader header{
            .Version = CookedAudioVersion,
            .Storage = static_cast<u32>(CookedAudioStorage::Pcm),
            .SampleFormat = static_cast<u32>(decoded->SampleFormat),
            .Codec = static_cast<u32>(CookedAudioCodec::None),
            .SampleRate = decoded->SampleRate,
            .Channels = decoded->Channels,
            .FrameCount = decoded->FrameCount,
        };
        return PackBlob(header, decoded->Payload);
    }
}
