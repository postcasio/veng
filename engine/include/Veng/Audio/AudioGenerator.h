#pragma once

#include <Veng/Veng.h>
#include <Veng/Audio/AudioBus.h>
#include <Veng/Audio/TripleBuffer.h>

#include <type_traits>

namespace Veng::Audio
{
    /// @brief A real-time source the mixer pulls samples from on demand.
    ///
    /// Where an AudioClip hands the mixer a finished buffer, a generator is called each block to
    /// synthesize the next samples. The caller implements Render and owns the object; the engine
    /// holds a borrowed pointer while the voice is live (see AudioEngine::PlayGenerator).
    ///
    /// @warning Render runs on the real-time mixing thread. It must be lock-free, allocation-free,
    ///          and touch no engine state — the same contract the voice snapshot rests on. Live
    ///          state reaches it only through a GeneratorParams block, never a direct call.
    struct IAudioGenerator
    {
        /// @brief Destroys the generator.
        virtual ~IAudioGenerator() = default;

        /// @brief Fills @p frames × @p channels interleaved float samples.
        ///
        /// Called on the real-time mixing thread. It must not lock, allocate, or call any engine
        /// API; it reads its drive parameters through a GeneratorParams block latched inside this
        /// call. The engine drives a voice at the channel count the voice declared through
        /// GeneratorVoiceParams::Channels — 1 for the default mono voice, 2 for a voice registered
        /// stereo (which must be non-spatial: a spatialized voice is mono-source-then-pan). So a
        /// mono voice writes @p frames samples and a stereo voice writes @p frames × 2 interleaved
        /// samples, at @p sampleRate.
        /// @param out        Destination for interleaved samples, @p frames × @p channels long.
        /// @param frames     Number of sample frames to produce.
        /// @param channels   Channel count to interleave (1 for a mono voice, 2 for a stereo one).
        /// @param sampleRate The output sample rate in Hz.
        virtual void Render(f32* out, u32 frames, u32 channels, u32 sampleRate) = 0;
    };

    /// @brief A lock-free triple-buffered parameter block driving a generator's synthesis.
    ///
    /// The sanctioned seam for live state to reach a generator's Render across the thread boundary:
    /// the main/View thread writes a whole Params through Set, the real-time Render latches and
    /// reads the newest through Get. It is the same triple-buffer primitive as the voice snapshot,
    /// scoped to one voice's parameters, so Set and Get running at unrelated rates never tear and
    /// the real-time reader never spins. Position is never a generator param — a spatial generator
    /// is placed with AudioEngine::SetVoicePose; a param block carries synthesis drive only.
    ///
    /// @tparam Params A trivially-copyable POD; three are held resident.
    template <class Params>
    class GeneratorParams
    {
    public:
        static_assert(std::is_trivially_copyable_v<Params>,
                      "GeneratorParams<Params> requires a trivially-copyable POD");

        /// @brief Publishes a new parameter set (main/View thread).
        ///
        /// Writes a free back buffer and publishes its index atomically; a later Get latches it. No
        /// lock is taken and the call never blocks.
        /// @param params The new parameters.
        void Set(const Params& params)
        {
            m_Buffer.BackBuffer() = params;
            m_Buffer.Publish();
        }

        /// @brief Latches and returns the newest published parameters (real-time thread).
        ///
        /// Returns the most recently published set, or a default-constructed Params before any Set.
        /// Lock-free and wait-free; it is the only sanctioned way Render reads its drive state.
        /// @return The newest parameters.
        [[nodiscard]] Params Get() const
        {
            m_Buffer.FetchNewest();
            return m_Buffer.FrontBuffer();
        }

    private:
        /// @brief The snapshot bridge; mutable so a const Get may advance the read index.
        mutable TripleBuffer<Params> m_Buffer;
    };

    /// @brief Registration parameters of a generator voice (AudioEngine::PlayGenerator).
    ///
    /// Describes how the voice mixes and, when Spatial, where it starts and how it rolls off. A
    /// spatial generator is then moved each frame with AudioEngine::SetVoicePose and shares the clip
    /// spatialization path; a non-spatial generator routes to its bus at Gain with no attenuation.
    struct GeneratorVoiceParams
    {
        /// @brief The bus the voice mixes into.
        AudioBus Bus = AudioBus::SFX;
        /// @brief Whether the voice is placed and spatialized against the listener.
        bool Spatial = false;
        /// @brief Rendered channel count: 1 (mono, the default) or 2 (an interleaved stereo image).
        ///
        /// A stereo voice's two channels are authored by the generator and mixed straight to the
        /// stereo bus, bypassing the pan stage. Stereo requires Spatial == false — a stereo point
        /// source has no defined pan — so PlayGenerator rejects a Spatial stereo request.
        u32 Channels = 1;
        /// @brief Linear gain applied before spatialization; 0 = silent, 1 = unity.
        f32 Gain = 1.0f;
        /// @brief Base playback pitch (resample ratio); Doppler multiplies this for a spatial voice.
        f32 Pitch = 1.0f;
        /// @brief Initial world position (Spatial voices); moved later with SetVoicePose.
        vec3 Position{0.0f};
        /// @brief Initial world velocity for Doppler, units per second (Spatial voices).
        vec3 Velocity{0.0f};
        /// @brief Distance at or within which the voice plays at full Gain (Spatial voices).
        f32 MinDistance = 1.0f;
        /// @brief Distance at or beyond which the voice is silent (Spatial voices).
        f32 MaxDistance = 50.0f;
        /// @brief Occlusion low-pass drive, 0 = clear to 1 = fully occluded (Spatial voices).
        f32 OcclusionFactor = 0.0f;
    };
}
