#pragma once

#include <Veng/Veng.h>
#include <Veng/Audio/AudioBuffer.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetType.h>

#include <span>

namespace Veng::Audio
{
    /// @brief How an AudioClip holds its samples: decoded PCM, or an encoded stream.
    ///
    /// Mirrors CookedAudioStorage; the integer values are the cooked ones.
    enum class AudioStorage : u32
    {
        /// @brief Decoded interleaved PCM, ready to mix — the short-effect form.
        Pcm = 0,
        /// @brief An encoded bitstream decoded incrementally at play time — the long-music form.
        Encoded = 1,
    };

    /// @brief The codec of an Encoded clip's payload.
    ///
    /// Mirrors CookedAudioCodec; the integer values are the cooked ones.
    enum class AudioCodec : u32
    {
        /// @brief No encoded codec — a Pcm clip reports this.
        None = 0,
        /// @brief Ogg Vorbis, decoded incrementally by the vendored decoder.
        Vorbis = 1,
    };

    /// @brief An incremental Ogg Vorbis decoder over an in-memory bitstream.
    ///
    /// Wraps the vendored stb_vorbis decoder behind the Native idiom: no decoder type appears in
    /// this header. It borrows the encoded bytes it is opened over — the buffer (an AudioClip's
    /// resident payload) must outlive the decoder — and yields interleaved float frames on demand.
    /// The decode is CPU work suitable for a worker thread; it must never run on the audio callback
    /// thread, which reads only ready samples.
    class VorbisMemoryDecoder
    {
    public:
        /// @brief Opens a decoder over an in-memory Vorbis bitstream.
        /// @param encoded The Vorbis bytes; must outlive the returned decoder.
        /// @return The decoder, or a message if the bitstream is not decodable.
        static Result<Unique<VorbisMemoryDecoder>> Open(std::span<const u8> encoded);

        /// @brief Destroys the decoder and its backend state.
        ~VorbisMemoryDecoder();

        VorbisMemoryDecoder(const VorbisMemoryDecoder&) = delete;
        VorbisMemoryDecoder& operator=(const VorbisMemoryDecoder&) = delete;

        /// @brief Decodes the next frames into an interleaved float buffer.
        ///
        /// Reads at most out.size() / Channels() frames, writing Channels() interleaved samples per
        /// frame. Returns the number of frames written; 0 marks the end of the stream.
        /// @param out Destination for interleaved samples; its length must be a multiple of Channels().
        /// @return The number of sample frames written (0 at end of stream).
        [[nodiscard]] u64 Read(std::span<f32> out);

        /// @brief Returns the channel count.
        [[nodiscard]] u32 Channels() const { return m_Channels; }
        /// @brief Returns the sample rate in Hz.
        [[nodiscard]] u32 SampleRate() const { return m_SampleRate; }

    private:
        VorbisMemoryDecoder();

        /// @brief The stb_vorbis handle, hidden behind the Native idiom.
        struct Native;
        /// @brief Owned backend decoder state.
        Unique<Native> m_Native;
        /// @brief Channel count read from the bitstream header.
        u32 m_Channels = 0;
        /// @brief Sample rate read from the bitstream header.
        u32 m_SampleRate = 0;
    };

    /// @brief A playable sound loaded by AssetId, in the mixer's native float format.
    ///
    /// A CPU-only asset (no GPU resource) that carries either decoded PCM (Storage::Pcm — a
    /// resident AudioBuffer a voice reads directly) or an encoded Vorbis stream (Storage::Encoded —
    /// resident bytes an incremental decoder consumes at play time). The cook-time mode chose which;
    /// both describe the same signal, so SampleRate/Channels/FrameCount are meaningful either way.
    class AudioClip
    {
    public:
        /// @brief Decodes a cooked audio blob into a clip.
        ///
        /// Reads a CookedAudioHeader, rejects a version or format mismatch with a message, and builds
        /// the resident form: a Pcm clip's samples are converted to a float AudioBuffer; an Encoded
        /// clip keeps the encoded payload for later incremental decode. No GPU work and no device.
        /// @param cooked The whole cooked blob (header + payload).
        /// @return The clip, or a message describing why the blob is not a valid audio clip.
        static Result<Ref<AudioClip>> Decode(std::span<const u8> cooked);

        /// @brief Wraps a runtime-built PCM buffer as a resident Pcm clip.
        ///
        /// The code-built path behind AudioEngine::CreateClip: the resulting clip is a Pcm clip in
        /// every respect except provenance, indistinguishable downstream from a cooked one.
        /// @param buffer The resident float PCM (must be non-null).
        /// @return The clip.
        static Ref<AudioClip> CreatePcm(Ref<AudioBuffer> buffer);

        /// @brief Returns how the clip stores its samples.
        [[nodiscard]] AudioStorage Storage() const { return m_Storage; }
        /// @brief Returns the codec of an Encoded clip's payload (None for a Pcm clip).
        [[nodiscard]] AudioCodec Codec() const { return m_Codec; }
        /// @brief Returns the sample rate of the decoded signal, in Hz.
        [[nodiscard]] u32 SampleRate() const { return m_SampleRate; }
        /// @brief Returns the channel count of the decoded signal.
        [[nodiscard]] u32 Channels() const { return m_Channels; }
        /// @brief Returns the number of sample frames in the decoded signal.
        [[nodiscard]] u64 FrameCount() const { return m_FrameCount; }

        /// @brief Returns the resident playable buffer, or null for an Encoded clip.
        [[nodiscard]] const Ref<AudioBuffer>& Buffer() const { return m_Buffer; }
        /// @brief Returns the resident encoded bytes, or an empty span for a Pcm clip.
        [[nodiscard]] std::span<const u8> Encoded() const { return m_Encoded; }

        /// @brief Opens an incremental decoder over an Encoded clip's resident bytes.
        ///
        /// The returned decoder borrows this clip's payload, so the clip must outlive it. Calling it
        /// on a Pcm clip is a usage error (there is nothing to decode) and returns a message.
        /// @return A decoder over the encoded stream, or a message.
        [[nodiscard]] Result<Unique<VorbisMemoryDecoder>> OpenDecoder() const;

    private:
        AudioClip() = default;

        /// @brief How the clip stores its samples.
        AudioStorage m_Storage = AudioStorage::Pcm;
        /// @brief Codec of the encoded payload (None for a Pcm clip).
        AudioCodec m_Codec = AudioCodec::None;
        /// @brief Sample rate of the decoded signal, in Hz.
        u32 m_SampleRate = 0;
        /// @brief Channel count of the decoded signal.
        u32 m_Channels = 0;
        /// @brief Number of sample frames in the decoded signal.
        u64 m_FrameCount = 0;
        /// @brief The resident playable buffer (Pcm clips only).
        Ref<AudioBuffer> m_Buffer;
        /// @brief The resident encoded bytes (Encoded clips only).
        vector<u8> m_Encoded;
    };
}

namespace Veng
{
    /// @brief AssetTypeTrait specialization mapping AudioClip to AssetTypes::AudioClip.
    template <>
    struct AssetTypeTrait<Audio::AudioClip>
    {
        /// @brief The asset type tag for AudioClip.
        static constexpr AssetTypeId Type = AssetTypes::AudioClip;
    };
}
