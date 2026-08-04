#pragma once

#include <Veng/Veng.h>
#include <Veng/Audio/Voice.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Scene/SceneSystem.h>

#include <unordered_map>
#include <unordered_set>

namespace Veng::Audio
{
    class AudioEngine;

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

    /// @brief The listener pose the AudioSystem spatializes every voice against.
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

namespace Veng
{
    class Scene;

    /// @brief View-phase system that places, spatializes, and mixes the scene's AudioSources.
    ///
    /// Runs in the View phase so it reads the same interpolated poses the renderer draws from — a
    /// sound sits where its emitter is drawn, not a fraction of a tick ahead. Each update it resolves
    /// the single AudioListener's live scene Transform (a listener at the origin when the scene has
    /// none, so non-spatial sound still plays), walks View<Transform, AudioSource> up to a voice cap,
    /// computes each voice's distance attenuation, pan, Doppler pitch, reverb send, and occlusion
    /// low-pass drive, and drives the AudioEngine's voice table (which publishes the immutable
    /// snapshot to the device). PlayOnStart sources begin with the simulation, looping sources
    /// persist, and a finished non-looping source is dropped once the device reports it retired
    /// through the lock-free retired-voice channel. When active sources exceed the cap the loudest
    /// after attenuation survive, matching the renderer's light clamp.
    ///
    /// The engine it publishes through is set by the host (SetEngine); with none set the system is an
    /// inert no-op, so a device-less or headless scene ticks it harmlessly.
    class AudioSystem final : public SceneSystem
    {
    public:
        /// @brief Returns Phase::View — audio is placed against the poses the frame draws.
        [[nodiscard]] Phase GetPhase() const override { return Phase::View; }

        /// @brief Sets the audio engine the system publishes voices through (null disables it).
        /// @param engine  The device's mixer-facing engine, or null to make the system inert.
        void SetEngine(Audio::AudioEngine* engine) { m_Engine = engine; }

        /// @brief Sets the maximum number of concurrent source voices the system holds.
        /// @param cap  The voice cap; the loudest-after-attenuation sources survive when exceeded.
        void SetVoiceCap(u32 cap) { m_VoiceCap = cap; }

        /// @brief Resets the system's voice bookkeeping for a fresh play session.
        /// @param scene    The scene the system operates over.
        /// @param context  Per-tick services (unused).
        void OnStart(Scene& scene, const SystemContext& context) override;

        /// @brief Places, spatializes, caps, and publishes the scene's AudioSource voices.
        /// @param scene    The scene whose AudioSources are placed.
        /// @param delta    Time in seconds since the previous tick.
        /// @param context  Per-tick services (carries the interpolation Alpha).
        void OnUpdate(Scene& scene, f32 delta, const SystemContext& context) override;

        /// @brief Stops every held voice and clears the bookkeeping.
        /// @param scene    The scene the system operates over.
        /// @param context  Per-tick services (unused).
        void OnStop(Scene& scene, const SystemContext& context) override;

        /// @brief Returns whether the system currently holds a live voice for an entity (test seam).
        [[nodiscard]] bool HasVoice(Entity entity) const { return m_Voices.contains(entity); }

        /// @brief Returns the last world position the system resolved a source at (test seam).
        ///
        /// The interpolated pose the voice was placed at this update, or nullopt for a source the
        /// system did not place.
        [[nodiscard]] optional<vec3> GetDebugSourcePosition(Entity entity) const;

    private:
        /// @brief The mixer-facing engine voices publish through, or null when the system is inert.
        Audio::AudioEngine* m_Engine = nullptr;
        /// @brief The concurrent source-voice cap; the loudest survive when exceeded.
        u32 m_VoiceCap = 32;
        /// @brief The live voice each playing source holds, keyed by source entity.
        std::unordered_map<Entity, Audio::VoiceHandle> m_Voices;
        /// @brief Non-looping sources that have finished, so they are not restarted.
        std::unordered_set<Entity> m_Finished;
        /// @brief Each placed source's previous-frame world position, for velocity (Doppler).
        std::unordered_map<Entity, vec3> m_SourcePosition;
        /// @brief The listener's previous-frame world position, for its velocity.
        vec3 m_ListenerPosition{0.0f};
        /// @brief Whether m_ListenerPosition holds a valid previous sample.
        bool m_HasListenerPosition = false;
    };
}

VE_SYSTEM(::Veng::AudioSystem, 0xD6C922D9005BF08FULL, "Audio");
