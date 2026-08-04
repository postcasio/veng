#pragma once

#include <Veng/Veng.h>

#include <span>

namespace Veng::Audio
{
    /// @brief The format of a code-built sample buffer handed to AudioEngine::CreateClip.
    struct AudioBufferFormat
    {
        /// @brief Sample rate in Hz; 0 asks the engine to use the device's output rate.
        u32 SampleRate = 0;
        /// @brief Channel count of the interleaved samples (1 = mono, 2 = stereo).
        u32 Channels = 1;
    };

    /// @brief An immutable, ref-counted block of interleaved 32-bit float PCM.
    ///
    /// A voice reads its samples from an AudioBuffer. The buffer is the reclamation unit the
    /// mixing thread references through raw pointers in the published snapshot: it is freed only
    /// once the real-time generation counter shows the callback cannot reach it (see
    /// AudioEngine::StopVoice and the module CLAUDE.md). It carries no device or backend state and
    /// is safe to build on any thread.
    class AudioBuffer
    {
    public:
        /// @brief Builds a buffer by copying interleaved float PCM.
        /// @param interleaved The source samples, channel-interleaved (length must be a multiple
        ///        of @p channels).
        /// @param channels    Channel count (1 = mono, 2 = stereo).
        /// @param sampleRate   Sample rate of the source, in Hz.
        /// @return A shared, immutable buffer.
        static Ref<AudioBuffer> Create(std::span<const f32> interleaved, u32 channels,
                                       u32 sampleRate);

        /// @brief Returns the interleaved sample span.
        [[nodiscard]] std::span<const f32> Samples() const { return m_Samples; }
        /// @brief Returns the number of sample frames (interleaved-length / channels).
        [[nodiscard]] u64 FrameCount() const { return m_FrameCount; }
        /// @brief Returns the channel count.
        [[nodiscard]] u32 Channels() const { return m_Channels; }
        /// @brief Returns the source sample rate in Hz.
        [[nodiscard]] u32 SampleRate() const { return m_SampleRate; }

    private:
        AudioBuffer(std::span<const f32> interleaved, u32 channels, u32 sampleRate);

        /// @brief Owned interleaved PCM.
        vector<f32> m_Samples;
        /// @brief Number of sample frames.
        u64 m_FrameCount = 0;
        /// @brief Channel count.
        u32 m_Channels = 0;
        /// @brief Source sample rate in Hz.
        u32 m_SampleRate = 0;
    };
}
