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
        /// @brief The bus the voice mixes into.
        Audio::AudioBus Bus = Audio::AudioBus::SFX;
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

    /// @brief The level's authored initial background-music track, read on world start.
    ///
    /// Placed on a level's settings entity, this is the "this level starts with this music" the
    /// AudioSystem hands to the music director once when the simulation starts (at the authored
    /// fade). Changing the track at runtime is the director's Set call; a level with no MusicState
    /// simply starts silent on the Music bus.
    struct MusicState
    {
        /// @brief The track the director plays when the level starts.
        AssetHandle<Audio::AudioClip> Track;
        /// @brief The fade-in duration in seconds; 0 starts the track at full gain.
        f32 FadeSeconds = 0.0f;
        /// @brief Whether the track loops.
        bool Loop = true;
    };
}

VE_ENUM(::Veng::Audio::AudioBus, 0x6200F1CAA558FF3DULL)
VE_ENUMERATOR(Master)
VE_ENUMERATOR(Music)
VE_ENUMERATOR(SFX)
VE_ENUMERATOR(UI)
VE_ENUMERATOR(Ambience)
VE_ENUM_END();

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
VE_REFLECT_END();
