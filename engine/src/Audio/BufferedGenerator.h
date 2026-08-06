#pragma once

#include <Veng/Veng.h>
#include <Veng/Audio/AudioGenerator.h>

#include "SpscRing.h"

#include <atomic>

namespace Veng::Audio
{
    /// @brief The smallest and largest ahead-of-time buffer depth a buffered voice may request, in
    ///        seconds.
    ///
    /// The depth is the consumer's policy — @ref GeneratorVoiceParams::BufferSeconds — because it is a
    /// latency-versus-headroom trade the consumer owns, not the engine: a deeper ring survives a longer
    /// synthesis stall but plays a parameter change that much later, and how much of each a voice wants
    /// depends on the voice. The request is clamped to this band before the ring is sized, so a
    /// consumer can neither ask for a degenerate ring nor a runaway allocation.
    inline constexpr f32 BufferedGeneratorMinSeconds = 0.05f;
    inline constexpr f32 BufferedGeneratorMaxSeconds = 4.0f;

    /// @brief How many frames the fill thread renders from a generator per pass while topping a ring.
    inline constexpr u32 BufferedGeneratorChunkFrames = 4096;

    /// @brief A buffered generator voice source: an IAudioGenerator rendered off the real-time thread.
    ///
    /// The opt-in latency-tolerant path beside the on-real-time-thread generator. The audio fill
    /// thread owns the borrowed generator and keeps @ref Ring topped with the interleaved samples it
    /// renders at the device rate; the real-time callback only drains @ref Ring — never rendering,
    /// never locking, never allocating. An empty ring is an underrun the callback fills with silence.
    /// There is no end: a generator is unbounded, so the fill thread refills forever until a Remove
    /// drops it. A buffered voice is non-spatial by construction, so the ring carries the final image
    /// (no pan, no Doppler, no occlusion applied on drain).
    ///
    /// Reclamation is a dual handshake, exactly the streaming voice's. The callback references
    /// @ref Ring through the published snapshot, so the object outlives any frame that can name it
    /// (the real-time serial handshake); the fill thread renders the borrowed generator, which the
    /// main thread drops only by posting an ordered Remove — the fill thread stops touching it and
    /// sets @ref ReleasedByDecoder, and the wrapper is freed only once both threads are provably past
    /// it. Because the generator is caller-owned, StopVoice additionally blocks on both parties before
    /// returning, so the caller may free the borrowed generator with no use-after-free.
    struct BufferedGenerator
    {
        /// @brief The rendered-PCM channel: fill thread pushes interleaved samples, callback pops them.
        ///        PlayGenerator sizes it to the voice's requested BufferSeconds (rounded up to a power
        ///        of two) before either thread touches it; it is never reallocated after.
        SpscRingDynamic<f32> Ring;

        /// @brief Fill thread → main: this voice is out of the fill thread's working set, safe to free.
        std::atomic<bool> ReleasedByDecoder{false};

        /// @brief The borrowed sample source; touched only by the fill thread after registration. Not owned.
        IAudioGenerator* Generator = nullptr;
        /// @brief The rendered channel count: 1 (mono) or 2 (interleaved stereo).
        u32 Channels = 1;
        /// @brief The device sample rate the generator renders at, in Hz.
        u32 SampleRate = 0;
        /// @brief Interleaved render scratch (BufferedGeneratorChunkFrames * Channels), fill-thread only.
        vector<f32> RenderScratch;
    };

    /// @brief A command the main thread posts to the fill thread over a lock-free ring.
    struct BufferedGeneratorCommand
    {
        /// @brief What the fill thread does with @ref Generator.
        enum class Kind : u8
        {
            /// @brief Add the voice to the fill thread's working set and start filling its ring.
            Add,
            /// @brief Drop the voice from the working set and acknowledge through ReleasedByDecoder.
            Remove,
        };

        /// @brief The command kind.
        Kind Op = Kind::Add;
        /// @brief The voice the command applies to.
        BufferedGenerator* Generator = nullptr;
    };
}
