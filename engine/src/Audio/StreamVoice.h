#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Audio/AudioClip.h>

#include "SpscRing.h"

#include <atomic>

namespace Veng::Audio
{
    /// @brief Ring capacity for a stream voice's decoded-PCM channel, in mono samples (a power of two).
    ///
    /// About 0.68 s of headroom at 48 kHz — deep enough that the decode thread's coarse wake cadence
    /// never starves the real-time reader, shallow enough that a handful of concurrent streams cost
    /// little memory.
    inline constexpr usize StreamRingCapacity = 1u << 15;

    /// @brief How many frames the decode thread pulls from a decoder per Read while topping a ring.
    inline constexpr u32 StreamDecodeChunkFrames = 4096;

    /// @brief A streaming voice source: an encoded clip decoded off the real-time thread into a ring.
    ///
    /// The third voice source beside a resident PCM buffer and an IAudioGenerator. A non-real-time
    /// decode thread owns the VorbisMemoryDecoder and keeps @ref Ring topped with mono samples at the
    /// clip's sample rate; the real-time callback only drains @ref Ring — never decoding, never
    /// locking, never allocating. An empty ring is an underrun the callback fills with silence; the
    /// decode thread seeks a looping stream back to the start at its end (so the loop point has no
    /// gap) and marks @ref AtEnd on a finite stream so the callback retires it once the ring drains.
    ///
    /// Reclamation is a dual handshake. The callback references @ref Ring through the published
    /// snapshot, so the object outlives any frame that can name it (the real-time serial handshake);
    /// the decode thread references it through its own working set, which the main thread mutates
    /// only by posting an ordered Remove command — the decode thread drops the voice and sets
    /// @ref ReleasedByDecoder, and the object is freed only once both threads are provably past it.
    struct StreamVoice
    {
        /// @brief The decoded-PCM channel: decode thread pushes mono samples, callback pops them.
        SpscRing<f32, StreamRingCapacity> Ring;
        /// @brief Set by the decode thread at a finite stream's end; the callback retires on it.
        std::atomic<bool> AtEnd{false};

        /// @brief Decode → main: this voice is out of the decode thread's working set, safe to free.
        std::atomic<bool> ReleasedByDecoder{false};

        /// @brief The clip handle, held so its encoded bytes outlive the decoder that borrows them.
        AssetHandle<AudioClip> Clip;
        /// @brief The incremental decoder; touched only by the decode thread after registration.
        ///
        /// Declared after Clip so it destructs first — the decoder borrows the clip's bytes.
        Unique<VorbisMemoryDecoder> Decoder;
        /// @brief Whether the stream loops (the decode thread seeks to start instead of ending).
        bool Loop = false;
        /// @brief The decoded signal's channel count (collapsed to mono into the ring).
        u32 Channels = 1;
        /// @brief The decoded signal's sample rate in Hz (the ring is at this rate).
        u32 SampleRate = 0;
        /// @brief Interleaved decode scratch (StreamDecodeChunkFrames * Channels), decode-thread only.
        vector<f32> DecodeScratch;
    };

    /// @brief A command the main thread posts to the decode thread over a lock-free ring.
    struct StreamCommand
    {
        /// @brief What the decode thread does with @ref Stream.
        enum class Kind : u8
        {
            /// @brief Add the voice to the decode thread's working set and start filling its ring.
            Add,
            /// @brief Drop the voice from the working set and acknowledge through ReleasedByDecoder.
            Remove,
        };

        /// @brief The command kind.
        Kind Op = Kind::Add;
        /// @brief The voice the command applies to.
        StreamVoice* Stream = nullptr;
    };
}
