#include <Veng/Audio/AudioClip.h>

#include <cstring>
#include <limits>

#include <fmt/format.h>

#include <Veng/Assert.h>
#include <Veng/Asset/CookedBlobs.h>

// stb_vorbis is compiled once in src/Vendor/StbVorbis.cpp; here we take declarations only. The
// config macros match that TU so the declared and defined surfaces agree.
#define STB_VORBIS_NO_STDIO
#define STB_VORBIS_HEADER_ONLY
#include "stb_vorbis.c"

namespace Veng::Audio
{
    struct VorbisMemoryDecoder::Native
    {
        /// @brief The vendored decoder over the borrowed in-memory bitstream.
        stb_vorbis* Handle = nullptr;

        ~Native()
        {
            if (Handle != nullptr)
            {
                stb_vorbis_close(Handle);
            }
        }
    };

    VorbisMemoryDecoder::VorbisMemoryDecoder() : m_Native(CreateUnique<Native>()) {}

    VorbisMemoryDecoder::~VorbisMemoryDecoder() = default;

    Result<Unique<VorbisMemoryDecoder>> VorbisMemoryDecoder::Open(const std::span<const u8> encoded)
    {
        if (encoded.empty())
        {
            return std::unexpected(string("vorbis decoder: empty bitstream"));
        }
        if (encoded.size() > static_cast<usize>(std::numeric_limits<int>::max()))
        {
            return std::unexpected(string("vorbis decoder: bitstream exceeds the decoder's range"));
        }

        int error = 0;
        stb_vorbis* handle = stb_vorbis_open_memory(
            encoded.data(), static_cast<int>(encoded.size()), &error, nullptr);
        if (handle == nullptr)
        {
            return std::unexpected(fmt::format(
                "vorbis decoder: failed to open bitstream (stb_vorbis error {})", error));
        }

        // The private constructor allocates the Native holder; take ownership of the handle into it.
        auto decoder = Unique<VorbisMemoryDecoder>(new VorbisMemoryDecoder());
        decoder->m_Native->Handle = handle;
        const stb_vorbis_info info = stb_vorbis_get_info(handle);
        decoder->m_Channels = static_cast<u32>(info.channels);
        decoder->m_SampleRate = info.sample_rate;
        return decoder;
    }

    u64 VorbisMemoryDecoder::Read(const std::span<f32> out)
    {
        if (m_Channels == 0 || out.size() < m_Channels)
        {
            return 0;
        }
        // stb_vorbis fills channels * frames floats and returns the frame count; cap the request to
        // whole frames the output span can hold.
        const int capacity = static_cast<int>(out.size());
        const int frames = stb_vorbis_get_samples_float_interleaved(
            m_Native->Handle, static_cast<int>(m_Channels), out.data(), capacity);
        return frames < 0 ? 0 : static_cast<u64>(frames);
    }

    void VorbisMemoryDecoder::SeekStart()
    {
        if (m_Native->Handle != nullptr)
        {
            stb_vorbis_seek_start(m_Native->Handle);
        }
    }

    namespace
    {
        /// @brief Reads a fixed-offset CookedAudioHeader, or a message when the blob is too short.
        Result<CookedAudioHeader> ReadHeader(const std::span<const u8> cooked)
        {
            if (cooked.size() < sizeof(CookedAudioHeader))
            {
                return std::unexpected(
                    string("audio clip: cooked blob smaller than CookedAudioHeader"));
            }
            CookedAudioHeader header;
            std::memcpy(&header, cooked.data(), sizeof(header));
            if (header.Version != CookedAudioVersion)
            {
                return std::unexpected(
                    fmt::format("audio clip: blob version {} does not match expected version {}",
                                header.Version, CookedAudioVersion));
            }
            return header;
        }

        /// @brief Builds the resident float buffer of a Pcm clip, validating the payload size.
        Result<Ref<AudioBuffer>> BuildPcm(const CookedAudioHeader& header,
                                          const std::span<const u8> payload)
        {
            const u64 sampleCount = header.FrameCount * header.Channels;
            const auto format = static_cast<CookedAudioSampleFormat>(header.SampleFormat);

            if (format == CookedAudioSampleFormat::F32)
            {
                if (payload.size() < sampleCount * sizeof(f32))
                {
                    return std::unexpected(
                        string("audio clip: cooked blob smaller than its declared PCM samples"));
                }
                vector<f32> samples(sampleCount);
                std::memcpy(samples.data(), payload.data(), sampleCount * sizeof(f32));
                return AudioBuffer::Create(samples, header.Channels, header.SampleRate);
            }

            if (format == CookedAudioSampleFormat::I16)
            {
                if (payload.size() < sampleCount * sizeof(i16))
                {
                    return std::unexpected(
                        string("audio clip: cooked blob smaller than its declared PCM samples"));
                }
                vector<f32> samples(sampleCount);
                for (u64 i = 0; i < sampleCount; ++i)
                {
                    i16 sample = 0;
                    std::memcpy(&sample, payload.data() + i * sizeof(i16), sizeof(i16));
                    samples[i] = static_cast<f32>(sample) / 32768.0f;
                }
                return AudioBuffer::Create(samples, header.Channels, header.SampleRate);
            }

            return std::unexpected(
                fmt::format("audio clip: unknown PCM sample format {}", header.SampleFormat));
        }
    }

    Result<Ref<AudioClip>> AudioClip::Decode(const std::span<const u8> cooked)
    {
        const Result<CookedAudioHeader> header = ReadHeader(cooked);
        if (!header)
        {
            return std::unexpected(header.error());
        }

        const std::span<const u8> payload = cooked.subspan(sizeof(CookedAudioHeader));
        const auto storage = static_cast<CookedAudioStorage>(header->Storage);

        auto clip = Ref<AudioClip>(new AudioClip());
        clip->m_SampleRate = header->SampleRate;
        clip->m_Channels = header->Channels;
        clip->m_FrameCount = header->FrameCount;

        if (storage == CookedAudioStorage::Pcm)
        {
            Result<Ref<AudioBuffer>> buffer = BuildPcm(*header, payload);
            if (!buffer)
            {
                return std::unexpected(buffer.error());
            }
            clip->m_Storage = AudioStorage::Pcm;
            clip->m_Codec = AudioCodec::None;
            clip->m_Buffer = std::move(*buffer);
            return clip;
        }

        if (storage == CookedAudioStorage::Encoded)
        {
            if (static_cast<CookedAudioCodec>(header->Codec) != CookedAudioCodec::Vorbis)
            {
                return std::unexpected(
                    fmt::format("audio clip: unknown encoded codec {}", header->Codec));
            }
            if (payload.empty())
            {
                return std::unexpected(string("audio clip: encoded clip carries no payload"));
            }
            clip->m_Storage = AudioStorage::Encoded;
            clip->m_Codec = AudioCodec::Vorbis;
            clip->m_Encoded.assign(payload.begin(), payload.end());
            return clip;
        }

        return std::unexpected(fmt::format("audio clip: unknown storage kind {}", header->Storage));
    }

    Ref<AudioClip> AudioClip::CreatePcm(Ref<AudioBuffer> buffer)
    {
        VE_ASSERT(buffer != nullptr, "AudioClip::CreatePcm requires a non-null buffer");
        auto clip = Ref<AudioClip>(new AudioClip());
        clip->m_Storage = AudioStorage::Pcm;
        clip->m_Codec = AudioCodec::None;
        clip->m_SampleRate = buffer->SampleRate();
        clip->m_Channels = buffer->Channels();
        clip->m_FrameCount = buffer->FrameCount();
        clip->m_Buffer = std::move(buffer);
        return clip;
    }

    Result<Unique<VorbisMemoryDecoder>> AudioClip::OpenDecoder() const
    {
        if (m_Storage != AudioStorage::Encoded)
        {
            return std::unexpected(string("audio clip: OpenDecoder on a non-encoded clip"));
        }
        return VorbisMemoryDecoder::Open(m_Encoded);
    }
}
