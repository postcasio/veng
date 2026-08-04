#pragma once

#include <Veng/Veng.h>
#include <Veng/Audio/AudioBus.h>
#include <Veng/Audio/AudioBuffer.h>
#include <Veng/Audio/Voice.h>

#include <array>

namespace Veng::Audio
{
    class AudioDevice;

    /// @brief The mixer-facing, main-thread API: buses and voices.
    ///
    /// The engine holds the authoritative bus parameters and voice table, publishes an immutable
    /// snapshot to the device's mixing thread each frame, and reaps finished voices and reclaimed
    /// resources through the device's generation counter. Every call is main-thread only; the
    /// real-time thread never touches it.
    class AudioEngine
    {
    public:
        /// @brief Constructs the engine over its owning device.
        /// @param device The device whose snapshot bridge and generation counter the engine drives.
        explicit AudioEngine(AudioDevice& device);

        AudioEngine(const AudioEngine&) = delete;
        AudioEngine& operator=(const AudioEngine&) = delete;

        /// @brief Sets a bus's linear gain.
        /// @param bus  The bus.
        /// @param gain Linear gain (clamped to >= 0).
        void SetBusGain(AudioBus bus, f32 gain);
        /// @brief Returns a bus's linear gain.
        [[nodiscard]] f32 GetBusGain(AudioBus bus) const;

        /// @brief Sets a bus's low-pass cutoff in Hz; 0 (or above Nyquist) bypasses the filter.
        /// @param bus       The bus.
        /// @param cutoffHz  Cutoff frequency in Hz.
        void SetBusLowpassCutoff(AudioBus bus, f32 cutoffHz);
        /// @brief Returns a bus's low-pass cutoff in Hz.
        [[nodiscard]] f32 GetBusLowpassCutoff(AudioBus bus) const;

        /// @brief Sets a bus's send level into the master reverb, 0..1.
        /// @param bus  The bus.
        /// @param send Send level, clamped to 0..1.
        void SetBusReverbSend(AudioBus bus, f32 send);
        /// @brief Returns a bus's send level into the master reverb.
        [[nodiscard]] f32 GetBusReverbSend(AudioBus bus) const;

        /// @brief Sets the master reverb parameters.
        /// @param params The reverb parameters.
        void SetReverbParams(const ReverbParams& params);
        /// @brief Returns the master reverb parameters.
        [[nodiscard]] ReverbParams GetReverbParams() const;

        /// @brief Registers a voice playing a buffer, arbitrating against the voice budget.
        ///
        /// Takes a free slot when one exists; when the budget is full, evicts the quietest active
        /// voice if the incoming voice is louder, and otherwise rejects the request. The evicted or
        /// rejected outcome is reported by an invalid handle.
        /// @param buffer The PCM source (held for the voice's lifetime).
        /// @param params The mix parameters.
        /// @return A handle to the voice, or an invalid handle if it was rejected.
        VoiceHandle AddVoice(const Ref<AudioBuffer>& buffer, const VoiceParams& params);

        /// @brief Returns whether a handle still names a live voice.
        /// @param voice The handle.
        [[nodiscard]] bool IsVoiceLive(VoiceHandle voice) const;

        /// @brief Updates a live voice's mix parameters (no effect on a stale handle).
        /// @param voice  The handle.
        /// @param params The new parameters.
        void SetVoiceParams(VoiceHandle voice, const VoiceParams& params);

        /// @brief Stops a voice and routes its source to reclamation (no effect on a stale handle).
        ///
        /// The source is freed only once the mixing thread's generation counter has passed the last
        /// snapshot that referenced the voice, so it can never be freed mid-mix.
        /// @param voice The handle.
        void StopVoice(VoiceHandle voice);

        /// @brief Returns the number of live voices.
        [[nodiscard]] usize GetActiveVoiceCount() const;

        /// @brief Publishes a snapshot of the current bus and voice state to the mixing thread.
        void Publish();

        /// @brief Reaps voices the mixing thread reported finished, routing sources to reclamation.
        void DrainRetired();

        /// @brief Frees reclaimed sources whose referencing generation the mixer has passed.
        void CollectDeferred();

    private:
        /// @brief One main-thread voice record.
        struct Voice
        {
            /// @brief Whether the slot holds a live voice.
            bool Active = false;
            /// @brief The slot's current generation.
            u32 Generation = 0;
            /// @brief The owned PCM source.
            Ref<AudioBuffer> Source;
            /// @brief The mix parameters.
            VoiceParams Params;
        };

        /// @brief A source awaiting reclamation once the mixer's generation passes it.
        struct Deferred
        {
            /// @brief The source to release.
            Ref<AudioBuffer> Source;
            /// @brief Free once the consumed generation exceeds this serial.
            u64 SafeAfterSerial = 0;
        };

        /// @brief Deactivates a slot and routes its source to reclamation.
        void RetireSlot(u32 slot);

        /// @brief The owning device.
        AudioDevice& m_Device;
        /// @brief Per-bus linear gain.
        std::array<f32, AudioBusCount> m_BusGain = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
        /// @brief Per-bus low-pass cutoff in Hz (0 = bypass).
        std::array<f32, AudioBusCount> m_BusLowpassCutoff = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        /// @brief Per-bus reverb send, 0..1.
        std::array<f32, AudioBusCount> m_BusReverbSend = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        /// @brief The master reverb parameters.
        ReverbParams m_Reverb;
        /// @brief The voice table.
        std::array<Voice, MaxVoices> m_Voices;
        /// @brief Sources awaiting reclamation.
        vector<Deferred> m_Deferred;
        /// @brief The last published snapshot serial.
        u64 m_PublishedSerial = 0;
        /// @brief The number of live voices.
        usize m_ActiveCount = 0;
    };
}
