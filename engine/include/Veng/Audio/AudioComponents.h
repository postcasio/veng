#pragma once

#include <Veng/Veng.h>
#include <Veng/Audio/AudioBus.h>
#include <Veng/Audio/AudioClip.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Reflection/Reflect.h>

namespace Veng
{
    /// @brief A sound placed in the world, consumed by the View-phase AudioSystem.
    ///
    /// References a loaded AudioClip and how it plays: which bus, its gain and pitch, whether it
    /// loops and whether it starts with the simulation. A Spatial source is attenuated, panned, and
    /// Doppler-shifted from the listener's pose (MinDistance/MaxDistance bound the rolloff and
    /// OcclusionFactor drives a low-pass); a non-spatial source routes straight to its bus at Gain
    /// with no attenuation or pan, which is what music and UI cues want. The clip's world position
    /// comes from the entity's Transform — never stored here — so a parented or moving emitter
    /// carries its sound with it.
    struct AudioSource
    {
        /// @brief The clip this source plays; a Pcm clip is placed directly, an unresident one is silent.
        AssetHandle<Audio::AudioClip> Clip;
        /// @brief The name of the bus the voice mixes into; resolved to a BusId when the voice plays.
        ///
        /// A bus name from the active bus graph (`"Music"`, `"SFX"`, a game-defined class). A name
        /// the active graph does not declare routes to Master with a one-time warning.
        string Bus = string(Audio::AudioBuses::SFXName);
        /// @brief Linear gain applied before spatialization; 0 = silent, 1 = unity.
        f32 Gain = 1.0f;
        /// @brief Base playback pitch (resample ratio); Doppler multiplies this for a spatial source.
        f32 Pitch = 1.0f;
        /// @brief Whether the voice loops; a looping source persists, a one-shot retires when it ends.
        bool Looping = false;
        /// @brief Whether the source begins playing when the simulation starts.
        bool PlayOnStart = false;
        /// @brief Whether the source is spatialized; false routes straight to the bus at Gain.
        bool Spatial = true;
        /// @brief Distance at or within which the source plays at full Gain (spatial only).
        f32 MinDistance = 1.0f;
        /// @brief Distance at or beyond which the source is silent (spatial only).
        f32 MaxDistance = 50.0f;
        /// @brief Occlusion low-pass drive, 0 = clear (bypass) to 1 = fully occluded (spatial only).
        ///
        /// The game supplies this — the engine mixes the low-pass but does not decide what occludes.
        f32 OcclusionFactor = 0.0f;
    };

    /// @brief Marks the entity whose Transform is the listener pose for spatialized audio.
    ///
    /// The AudioSystem resolves the single listener in the scene and spatializes every AudioSource
    /// relative to its position, orientation, and per-frame velocity (for Doppler). With no listener
    /// the system falls back to a listener at the origin, so non-spatial sound still plays.
    struct AudioListener
    {
        /// @brief Master gain applied to every voice this listener hears.
        f32 Gain = 1.0f;
    };

    /// @brief The scene's live background-music request.
    ///
    /// Present, it means "this scene wants this track"; absent, the scene wants none. The scene's
    /// AudioSystem submits it as its presentation scope's music request every View update, so any
    /// field may change at runtime — a new Track crossfades to it. The engine plays the request of
    /// the highest-Priority eligible scope (a scene on screen, running or paused; never one nothing
    /// presents), breaking ties toward the scene on the primary viewport, and returns to the next
    /// request when the winning scene ends, fading out when none remain. Placed on a level's settings
    /// entity; the first one the scene finds is the request.
    struct MusicState
    {
        /// @brief The track to play; an empty track asks for silence at this Priority.
        AssetHandle<Audio::AudioClip> Track;
        /// @brief The crossfade into this track in seconds (0 is a hard cut); also the fade-out when
        ///        this scene's music ends with no other request standing.
        f32 FadeSeconds = 0.0f;
        /// @brief Whether the track loops.
        bool Loop = true;
        /// @brief The request's priority: the highest among eligible scenes plays, so an overlay can
        ///        outrank the scene it covers.
        i32 Priority = 0;
    };
}

VE_REFLECT(::Veng::AudioSource, 0x473BC42991887B82ULL)
VE_FIELD(Clip, .DisplayName = "Clip")
VE_FIELD(Bus, .DisplayName = "Bus")
VE_FIELD(Gain, .DisplayName = "Gain", .Display = {.Min = 0.0})
VE_FIELD(Pitch, .DisplayName = "Pitch", .Display = {.Min = 0.0, .Step = 0.01})
VE_FIELD(Looping, .DisplayName = "Looping")
VE_FIELD(PlayOnStart, .DisplayName = "Play on Start")
VE_FIELD(Spatial, .DisplayName = "Spatial")
VE_FIELD(MinDistance, .DisplayName = "Min Distance", .Display = {.Min = 0.0},
         .VisibleIf = VE_WHEN(self.Spatial))
VE_FIELD(MaxDistance, .DisplayName = "Max Distance", .Display = {.Min = 0.0},
         .VisibleIf = VE_WHEN(self.Spatial))
VE_FIELD(OcclusionFactor, .DisplayName = "Occlusion",
         .Display = {.Min = 0.0, .Max = 1.0, .Step = 0.01}, .VisibleIf = VE_WHEN(self.Spatial))
VE_REFLECT_END();
// Not VE_REPLICATED: an AudioSource is authored placement plus a game-driven OcclusionFactor; the
// audio mix is a client-local presentation the View-phase AudioSystem derives, never on the wire.

VE_REFLECT(::Veng::AudioListener, 0x94FFDFB63B3607BBULL)
VE_FIELD(Gain, .DisplayName = "Gain", .Display = {.Min = 0.0})
VE_REFLECT_END();

VE_REFLECT(::Veng::MusicState, 0x52BEE4008DB11531ULL)
VE_FIELD(Track, .DisplayName = "Track")
VE_FIELD(FadeSeconds, .DisplayName = "Fade Seconds", .Display = {.Min = 0.0, .Step = 0.05})
VE_FIELD(Loop, .DisplayName = "Loop")
VE_FIELD(Priority, .DisplayName = "Priority")
VE_REFLECT_END();
