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
    struct AudioSource;

    namespace Audio
    {
        struct IAudioGenerator;
    }

    /// @brief View-phase system that places, spatializes, and mixes the scene's AudioSources.
    ///
    /// Runs in the View phase so it reads the same interpolated poses the renderer draws from — a
    /// sound sits where its emitter is drawn, not a fraction of a tick ahead. Each update it resolves
    /// the single AudioListener's live scene Transform (a listener at the origin when the scene has
    /// none, so non-spatial sound still plays), walks View<Transform, AudioSource> up to a voice cap,
    /// computes each voice's distance attenuation, pan, Doppler pitch, reverb send, and occlusion
    /// low-pass drive, and drives the AudioEngine's voice table (which publishes the immutable
    /// snapshot to the device). A source sounds while its Playing control is set: one authored or
    /// added playing starts on the first update that sees it, clearing Playing stops it, and a
    /// non-looping clip that finishes clears Playing, so setting it again replays it. When active
    /// sources exceed the cap the loudest after attenuation survive, matching the renderer's light
    /// clamp.
    ///
    /// A source carrying a Generator plays it through the same gather: it starts as a generator voice
    /// (spatial ones moved to the drawn pose each update), a replaced generator restarts the voice on
    /// the new one, and a restart on the same instance waits until the engine no longer holds it, so
    /// one generator never plays as two voices. A spatial source asking for a stereo or buffered
    /// generator starts nothing and warns once, as the engine would refuse it.
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
        /// @brief How a source asks to be voiced: its sample source and a generator voice's
        ///        registration. A live voice started under a different shape restarts.
        struct VoiceShape
        {
            /// @brief The generator played, compared by identity only; null for a clip voice.
            const Audio::IAudioGenerator* Generator = nullptr;
            /// @brief Whether a generator voice is registered positioned (moved by SetVoicePose).
            bool Spatial = false;
            /// @brief A generator voice's registered channel count.
            u32 Channels = 1;
            /// @brief Whether a generator voice is registered buffered.
            bool Buffered = false;
            /// @brief A buffered generator voice's registered depth in seconds.
            f32 BufferSeconds = 0.0f;

            /// @brief Compares every field.
            bool operator==(const VoiceShape&) const = default;
        };

        /// @brief The voice a playing source holds, and the shape it was started under.
        struct SourceVoice
        {
            /// @brief The live voice.
            Audio::VoiceHandle Voice;
            /// @brief The shape the voice was started under.
            VoiceShape Shape;
        };

        /// @brief Returns the shape a source asks to be voiced under; every clip source shares one,
        ///        since a clip voice is retuned whatever its fields.
        [[nodiscard]] static VoiceShape ShapeOf(const AudioSource& source);

        /// @brief The concurrent source-voice cap; the loudest survive when exceeded.
        u32 m_VoiceCap = 32;
        /// @brief The live voice each playing source holds, keyed by source entity.
        std::unordered_map<Entity, SourceVoice> m_Voices;
        /// @brief Sources already warned about asking for a generator voice the engine refuses.
        std::unordered_set<Entity> m_Rejected;
        /// @brief Each placed source's previous-frame world position, for velocity (Doppler).
        std::unordered_map<Entity, vec3> m_SourcePosition;
        /// @brief The listener's previous-frame world position, for its velocity.
        vec3 m_ListenerPosition{0.0f};
        /// @brief Whether m_ListenerPosition holds a valid previous sample.
        bool m_HasListenerPosition = false;
    };
}

VE_SYSTEM(::Veng::AudioSystem, 0xD6C922D9005BF08FULL, "Audio");
