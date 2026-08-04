#pragma once

#include <Veng/Veng.h>
#include <Veng/Audio/AudioBus.h>

namespace Veng::Audio
{
    /// @brief The device-wide voice budget.
    ///
    /// A single fixed cap governs the whole mix: authored sources, one-shots, the music director,
    /// and generator voices all arbitrate against it, so real-time CPU is bounded by this number,
    /// not by scene population. It sizes the fixed-capacity snapshot arrays, so it is a
    /// compile-time constant rather than a device knob.
    inline constexpr u32 MaxVoices = 128;

    /// @brief An opaque handle to a registered voice.
    ///
    /// A slot index plus a generation: the slot is reused as voices come and go, and the
    /// generation distinguishes a live voice from a stale handle to a since-retired one in the
    /// same slot.
    struct VoiceHandle
    {
        /// @brief The sentinel slot value of an invalid handle.
        static constexpr u32 InvalidSlot = 0xFFFFFFFFU;

        /// @brief The voice slot, or InvalidSlot.
        u32 Slot = InvalidSlot;
        /// @brief The generation the slot carried when this handle was minted.
        u32 Generation = 0;

        /// @brief Returns whether the handle names a slot (not that the voice is still live).
        [[nodiscard]] bool IsValid() const { return Slot != InvalidSlot; }

        /// @brief Value equality over slot and generation.
        bool operator==(const VoiceHandle&) const = default;
    };

    /// @brief The per-voice mix parameters, already spatialized by the caller.
    ///
    /// veng owns the spatialization DSP, so these are the final numbers the mixer consumes — the
    /// attenuation, pan, and Doppler pitch the caller computes are applied here, never by the
    /// backend.
    struct VoiceParams
    {
        /// @brief The bus this voice mixes into.
        AudioBus Bus = AudioBus::SFX;
        /// @brief Final linear gain (post-attenuation), 0 = silent, 1 = unity.
        f32 Gain = 1.0f;
        /// @brief Stereo pan, -1 = hard left, 0 = centre, +1 = hard right (equal-power).
        f32 Pan = 0.0f;
        /// @brief Resample ratio applied on top of the clip-rate conversion (Doppler pitch); 1 = none.
        f32 Pitch = 1.0f;
        /// @brief Occlusion low-pass amount, 0 = open (bypass), 1 = fully occluded.
        f32 Occlusion = 0.0f;
        /// @brief Send level into the master reverb, 0 = dry, 1 = full send.
        f32 ReverbSend = 0.0f;
        /// @brief Whether the voice loops (finite one-shots retire; loops never do).
        bool Loop = false;
    };

    /// @brief Parameters of the first-party master reverb node.
    struct ReverbParams
    {
        /// @brief Room size / feedback, 0 = small, 1 = large.
        f32 RoomSize = 0.5f;
        /// @brief High-frequency damping in the tail, 0 = bright, 1 = dark.
        f32 Damping = 0.5f;
        /// @brief Wet mix added to the master, 0 = reverb inaudible, 1 = full wet.
        f32 Wet = 0.0f;
        /// @brief Stereo width of the wet signal, 0 = mono, 1 = full width.
        f32 Width = 1.0f;
    };

    /// @brief A voice reported by the real-time thread as finished playing.
    struct RetiredVoice
    {
        /// @brief The slot the retired voice occupied.
        u32 Slot = VoiceHandle::InvalidSlot;
        /// @brief The generation of the retired voice.
        u32 Generation = 0;
    };
}
