#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Haptics/Haptics.h>
#include <Veng/Haptics/RumbleClip.h>
#include <Veng/Input.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Scene/Entity.h>

namespace Veng
{
    /// @brief Continuous rumble as authored data, played by the View-phase HapticsSystem.
    ///
    /// The haptics peer of AudioSource: a clip, where it plays and how strongly, on an entity of the
    /// scene that owns it. HapticsSystem advances it each frame and submits its level to its scene's
    /// presentation scope, so it holds while the world is paused, is silent while nothing presents
    /// the scene, and stops the moment the component or its entity goes — no handle, no stop call.
    ///
    /// **`Playing` is the one control.** Authored true, it plays from the first frame the system sees
    /// it (a spawn, or the component being added at runtime); a system sets it false to stop, ramping
    /// to silence over FadeOutSeconds, and true again to restart from the clip's start. The system
    /// clears it when a once-through clip ends. `Intensity` is live: a system may drive it every tick.
    ///
    /// Client-local presentation, not replicated.
    struct RumbleSource
    {
        /// @brief The clip; one still loading (or none) is silent.
        AssetHandle<Haptics::RumbleClip> Clip;
        /// @brief What it plays on: a seat of this scene, or a pad slot.
        Haptics::RumbleTargetKind Target = Haptics::RumbleTargetKind::Seat;
        /// @brief The seat's Viewer entity, for a Seat target; Entity::Null is the implicit seat.
        ///
        /// Resolved every frame through its SeatInput::Gamepad, so the rumble follows a reassignment
        /// and is silent while the seat has no pad.
        Entity Seat = Entity::Null;
        /// @brief The pad slot, for a Gamepad target: an application without seats.
        GamepadId Gamepad = GamepadId::None;
        /// @brief Scales every channel; 0 or more.
        f32 Intensity = 1.0f;
        /// @brief How the clip runs out: as the clip says, always wrapping, or once through.
        Haptics::RumbleLoop Loop = Haptics::RumbleLoop::FromClip;
        /// @brief Whether it plays; a rise restarts the clip, a fall fades it out.
        bool Playing = true;
        /// @brief Seconds it ramps to silence over when Playing goes false; 0 stops at once.
        f32 FadeOutSeconds = 0.0f;

        /// @brief Runtime: seconds into the clip, unwrapped. Not authored.
        f32 Time = 0.0f;
        /// @brief Runtime: the fade level, 1 while playing and falling to 0 after a stop. Not authored.
        f32 Fade = 0.0f;
        /// @brief Runtime: the Playing value HapticsSystem last saw, so it sees a rise or a fall.
        ///        Not authored.
        bool WasPlaying = false;
    };
}

VE_REFLECT(::Veng::RumbleSource, 0x8255C185FA51C610ULL)
VE_FIELD(Clip, .DisplayName = "Clip")
VE_FIELD(Target, .DisplayName = "Target")
VE_FIELD(Seat, .DisplayName = "Seat",
         .VisibleIf = VE_WHEN(self.Target == ::Veng::Haptics::RumbleTargetKind::Seat))
VE_FIELD(Gamepad, .DisplayName = "Gamepad",
         .VisibleIf = VE_WHEN(self.Target == ::Veng::Haptics::RumbleTargetKind::Gamepad))
VE_FIELD(Intensity, .DisplayName = "Intensity", .Display = {.Min = 0.0, .Step = 0.01})
VE_FIELD(Loop, .DisplayName = "Loop")
VE_FIELD(Playing, .DisplayName = "Playing")
VE_FIELD(FadeOutSeconds, .DisplayName = "Fade Out Seconds", .Display = {.Min = 0.0, .Step = 0.05})
VE_REFLECT_END();
// Not VE_REPLICATED: rumble is client-local presentation, and Time, Fade and WasPlaying are the
// HapticsSystem's own runtime state.
