#pragma once

#include <Veng/Veng.h>
#include <Veng/Audio/AudioBus.h>
#include <Veng/Audio/Voice.h>

#include <array>

namespace Veng::Audio
{
    /// @brief One voice as the real-time mixer sees it: an immutable, POD description.
    ///
    /// The source is a raw pointer into an AudioBuffer whose lifetime the reclamation handshake
    /// guarantees for as long as any published frame can reference it (see the module CLAUDE.md).
    /// The struct leaves room for a generator source without committing to it here.
    struct VoiceSnapshot
    {
        /// @brief Whether this slot holds a live voice.
        bool Active = false;
        /// @brief The voice generation, matching the RT-side playback cursor identity.
        u32 Generation = 0;

        /// @brief Interleaved PCM samples, or null.
        const f32* Pcm = nullptr;
        /// @brief Number of sample frames in @ref Pcm.
        u64 PcmFrameCount = 0;
        /// @brief Channel count of @ref Pcm.
        u32 PcmChannels = 0;
        /// @brief Sample rate of @ref Pcm, in Hz.
        u32 PcmSampleRate = 0;

        /// @brief The bus this voice mixes into.
        AudioBus Bus = AudioBus::SFX;
        /// @brief Final linear gain.
        f32 Gain = 1.0f;
        /// @brief Stereo pan, -1..+1.
        f32 Pan = 0.0f;
        /// @brief Resample ratio (Doppler pitch).
        f32 Pitch = 1.0f;
        /// @brief Occlusion low-pass amount, 0..1.
        f32 Occlusion = 0.0f;
        /// @brief Master reverb send, 0..1.
        f32 ReverbSend = 0.0f;
        /// @brief Whether the voice loops.
        bool Loop = false;
    };

    /// @brief The immutable per-block snapshot the main thread publishes to the mixing thread.
    ///
    /// POD, allocation-free, and fixed-capacity: it carries the whole mix state — bus parameters,
    /// the master reverb, and every active voice — so the callback reads exactly one object and
    /// touches no engine API.
    struct AudioFrame
    {
        /// @brief A monotonically increasing publish serial; drives the reclamation handshake.
        u64 Serial = 0;

        /// @brief Per-bus linear gain.
        std::array<f32, AudioBusCount> BusGain = {1.0f, 1.0f, 1.0f, 1.0f, 1.0f};
        /// @brief Per-bus low-pass cutoff in Hz; 0 (or above Nyquist) is bypass.
        std::array<f32, AudioBusCount> BusLowpassCutoff = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};
        /// @brief Per-bus send into the master reverb, 0..1.
        std::array<f32, AudioBusCount> BusReverbSend = {0.0f, 0.0f, 0.0f, 0.0f, 0.0f};

        /// @brief The master reverb parameters.
        ReverbParams Reverb;

        /// @brief Every voice slot; inactive slots have Active == false.
        std::array<VoiceSnapshot, MaxVoices> Voices;
    };
}
