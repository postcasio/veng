#pragma once

#include <Veng/Veng.h>
#include <Veng/Audio/Voice.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Scene/SceneSystem.h>

#include <unordered_map>
#include <unordered_set>

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
    /// It drives the engine through SystemContext::Audio, the facade bound to the scene's
    /// presentation scope, so its voices belong to the scene: a paused world's sources hold, an
    /// unpresented one's are silent, and a closed scene's stop. Each update it sets the scene's
    /// listener on that scope, which every PlayAt a system of this scene fires is spatialized
    /// against too, and submits the scene's MusicState as that scope's music request, which the engine
    /// arbitrates against every other scope's. The engine's once-per-frame advance
    /// (AudioEngine::Update) is the application's, not this system's.
    class AudioSystem final : public SceneSystem
    {
    public:
        /// @brief Returns Phase::View — audio is placed against the poses the frame draws.
        [[nodiscard]] Phase GetPhase() const override { return Phase::View; }

        /// @brief Sets the maximum number of concurrent source voices the system holds.
        /// @param cap  The voice cap; the loudest-after-attenuation sources survive when exceeded.
        void SetVoiceCap(u32 cap) { m_VoiceCap = cap; }

        /// @brief Resets the voice bookkeeping.
        /// @param scene    The scene the system operates over.
        /// @param context  Per-tick services (unused).
        void OnStart(Scene& scene, const SystemContext& context) override;

        /// @brief Places, spatializes, caps, and publishes the scene's AudioSource voices, and
        ///        submits the scene's MusicState (or its absence) as its scope's music request.
        /// @param scene    The scene whose AudioSources are placed.
        /// @param delta    Time in seconds since the previous tick.
        /// @param context  Per-tick services (carries the interpolation Alpha).
        void OnUpdate(Scene& scene, f32 delta, const SystemContext& context) override;

        /// @brief Stops every held voice, withdraws the scene's music request, and clears the
        ///        bookkeeping.
        /// @param scene    The scene the system operates over.
        /// @param context  Per-tick services (the scene's audio facade).
        void OnStop(Scene& scene, const SystemContext& context) override;

        /// @brief Returns whether the system currently holds a live voice for an entity (test seam).
        [[nodiscard]] bool HasVoice(Entity entity) const { return m_Voices.contains(entity); }

        /// @brief Returns the last world position the system resolved a source at (test seam).
        ///
        /// The interpolated pose the voice was placed at this update, or nullopt for a source the
        /// system did not place.
        [[nodiscard]] optional<vec3> GetDebugSourcePosition(Entity entity) const;

    private:
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
