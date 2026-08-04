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

    /// @brief The concurrent cap on engine-owned one-shot and positioned voices.
    ///
    /// Fire-and-forget voices (PlayOneShot) and imperatively-placed spatial voices (PlayAt) share
    /// this pool inside the wider MaxVoices budget: when it is full a new request evicts the
    /// quietest pooled voice if it is louder, and is otherwise rejected. The music director's
    /// crossfade pair is governed separately (at most two voices), not by this cap.
    inline constexpr u32 MaxOneShotVoices = 32;

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

    /// @brief Speed of sound in air, metres per second — the Doppler reference velocity.
    inline constexpr f32 DefaultSpeedOfSound = 343.0f;

    /// @brief Linear distance rolloff: full gain at/inside min, zero at/beyond max, monotone between.
    ///
    /// Pure math — no scene, no device — so it is the deterministic core both the audio system and
    /// the unit tests drive. A degenerate band (max <= min) is a hard cutoff at min.
    /// @param distance     Distance from listener to source, in world units.
    /// @param minDistance  Distance within which the source plays at full gain.
    /// @param maxDistance  Distance at or beyond which the source is silent.
    /// @return The attenuation factor in [0, 1].
    [[nodiscard]] f32 DistanceAttenuation(f32 distance, f32 minDistance, f32 maxDistance);

    /// @brief Equal-power stereo pan from the source's direction in the listener's frame.
    ///
    /// Projects the listener→source direction onto the listener's local right axis (its rotated +X):
    /// a source hard-right of the listener returns +1, hard-left -1, dead-ahead 0. Resolving in the
    /// listener's frame is what makes a turned listener hear world-left on its right. A source
    /// coincident with the listener is centred. Pure math — no scene, no device.
    /// @param listenerPosition  The listener's world position.
    /// @param listenerRotation  The listener's world rotation.
    /// @param sourcePosition    The source's world position.
    /// @return The pan in [-1, +1] (VoiceParams::Pan).
    [[nodiscard]] f32 StereoPan(vec3 listenerPosition, quat listenerRotation, vec3 sourcePosition);

    /// @brief The Doppler resample ratio from listener and source velocity along their line of sight.
    ///
    /// Returns (c + v_listener) / (c + v_source) where each velocity is the component along the
    /// listener→source direction: a source approaching the listener (or a listener approaching the
    /// source) raises the ratio above 1, receding lowers it below 1, and zero radial velocity leaves
    /// it exactly 1. Clamped to a sane band so a fast pass-by cannot produce an extreme pitch. Pure
    /// math — no scene, no device.
    /// @param listenerPosition  The listener's world position.
    /// @param listenerVelocity  The listener's world velocity, units per second.
    /// @param sourcePosition    The source's world position.
    /// @param sourceVelocity    The source's world velocity, units per second.
    /// @param speedOfSound      The reference speed of sound, units per second.
    /// @return The pitch ratio (VoiceParams::Pitch multiplier), clamped to [0.5, 2].
    [[nodiscard]] f32 DopplerRatio(vec3 listenerPosition, vec3 listenerVelocity,
                                   vec3 sourcePosition, vec3 sourceVelocity, f32 speedOfSound);

    /// @brief Distance-driven master reverb send: dry near the listener, wetter with distance.
    ///
    /// A bounded ramp over the source's [min, max] rolloff band — a distant source reads as more
    /// reverberant. Pure math — no scene, no device.
    /// @param distance     Distance from listener to source, in world units.
    /// @param minDistance  Distance within which the send is dry.
    /// @param maxDistance  Distance at or beyond which the send saturates.
    /// @return The reverb send in [0, 1] (VoiceParams::ReverbSend).
    [[nodiscard]] f32 ReverbSend(f32 distance, f32 minDistance, f32 maxDistance);

    /// @brief The listener pose voices are spatialized against.
    struct ListenerPose
    {
        /// @brief World position.
        vec3 Position{0.0f};
        /// @brief World rotation (its -Z is forward, +X right, matching the engine convention).
        quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
        /// @brief World velocity for Doppler, units per second (a per-frame difference).
        vec3 Velocity{0.0f};
        /// @brief Master gain applied to every voice.
        f32 Gain = 1.0f;
    };
}
