#pragma once

#include <Veng/Veng.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Scene/Entity.h>

namespace Veng
{
    /// @brief Camera-rig follow relationship: the target a camera entity trails and how.
    ///
    /// Read by the View-phase camera rig: each tick it reads the target's world Transform
    /// and writes the camera entity's Transform to trail it by Offset (the target's local
    /// frame), optionally smoothed by Damping. Target is a reflected Entity reference, so
    /// it remaps on prefab spawn like any intra-prefab reference. Because the rig runs in
    /// the View phase, the produced camera pose is purely local — never authoritative,
    /// never on the wire.
    struct CameraFollow
    {
        /// @brief The entity whose world Transform the camera trails, or Entity::Null for no follow.
        Entity Target = Entity::Null;
        /// @brief Position offset from the target, expressed in the target's local frame.
        vec3 Offset{0.0f, 5.0f, 10.0f};
        /// @brief Exponential-smoothing rate per second; 0 snaps the camera to the target each tick.
        f32 Damping = 0.0f;
        /// @brief Orbit pitch about the target, in radians.
        ///
        /// Tilts the camera up and down around the target without rotating the target. This is
        /// runtime view state driven by look input — not authored and not serialized; it starts
        /// at zero on spawn.
        f32 Pitch = 0.0f;
    };

    /// @brief Camera-rig first-person look: a yaw/pitch heading the rig writes as the
    /// entity's rotation.
    ///
    /// Read by the View-phase camera rig: each tick it clamps Pitch into ±PitchLimit and
    /// writes the entity's Transform rotation as yaw about world up composed with pitch
    /// about the yawed right axis. The entity's position is untouched — the camera sits
    /// where it is placed (or parented). A game's control system drives Yaw/Pitch from its
    /// look action with its own sensitivity, the way it drives CameraFollow::Pitch; the
    /// authored values are the spawn-time facing. Because the rig runs in the View phase,
    /// the produced rotation is purely local — never authoritative, never on the wire.
    struct CameraLook
    {
        /// @brief Heading about world up, in radians; positive turns left.
        f32 Yaw = 0.0f;
        /// @brief Elevation about the yawed right axis, in radians; positive looks up.
        f32 Pitch = 0.0f;
        /// @brief Maximum |Pitch| in radians; the rig clamps Pitch into this range each tick.
        f32 PitchLimit = 1.5f;
    };

    /// @brief Camera-rig orbit relationship: a camera that circles a focus point at a distance.
    ///
    /// Read by the View-phase camera rig: each tick it places the camera entity a Distance from
    /// Focus along the Yaw/Pitch heading and orients it back at Focus. Unlike CameraFollow, the
    /// orbit tracks a point rather than a target entity, so it is the rig any scene inspector,
    /// model viewer, or map/chart view uses. FocusTarget with a positive FocusDamping glides
    /// Focus toward the target over time, so recentering the view is smooth rather than an
    /// instant cut; a zero FocusDamping snaps Focus to FocusTarget each tick. Distance is held
    /// within [MinDistance, MaxDistance] and Pitch within ±PitchLimit — the pitch clamp keeps the
    /// pose off the pole where the look-at up vector collapses. Because the rig runs in the View
    /// phase, the produced camera pose is purely local — never authoritative, never on the wire.
    struct CameraOrbit
    {
        /// @brief The point orbited, in world space.
        vec3 Focus{0.0f, 0.0f, 0.0f};
        /// @brief Eye distance from Focus, clamped into [MinDistance, MaxDistance].
        f32 Distance = 10.0f;
        /// @brief Smallest allowed Distance, so the eye never reaches the focus.
        f32 MinDistance = 0.1f;
        /// @brief Largest allowed Distance.
        f32 MaxDistance = 1000.0f;
        /// @brief Heading about world up, in radians; positive turns the eye left around Focus.
        f32 Yaw = 0.0f;
        /// @brief Elevation about the yawed right axis, in radians; positive swings the eye
        ///        below Focus so the camera tilts up toward it (matching CameraLook::Pitch).
        f32 Pitch = 0.0f;
        /// @brief Maximum |Pitch| in radians; the rig clamps Pitch off the degenerate pole.
        f32 PitchLimit = 1.5f;
        /// @brief Where Focus glides toward when FocusDamping is positive, in world space.
        vec3 FocusTarget{0.0f, 0.0f, 0.0f};
        /// @brief Exponential-smoothing rate per second of Focus toward FocusTarget; 0 snaps.
        f32 FocusDamping = 0.0f;
    };

    /// @brief Camera-rig first-person relationship: an eye-anchored camera that yaws about the
    ///        target's up rather than the world's.
    ///
    /// Read by the View-phase camera rig: each tick it reads the target's resolved up — its
    /// CharacterState::Up when the target carries one and is not Seated, else the target's own
    /// transform up — and builds the camera basis against it, so the horizon stays level even where
    /// "up" is not a world constant (a curved habitat, a rotating deck). A seated target reads its
    /// own transform because its frame is the seat's, not the one it was last standing in; its
    /// CharacterState is not removed on entry and stops being advanced, so following it would hold
    /// the horizon to the ground the occupant walked in on. The eye sits at the target's position
    /// offset by EyeOffset in the target's local frame, or — when EyeSocket names a mesh socket on
    /// the target — at that socket's world position with EyeOffset applied on top. The heading is
    /// read from a sibling CameraLook (its accumulated Yaw about the up axis and Pitch about the
    /// horizon), so a control system drives look input into CameraLook exactly as it does for the
    /// plain look rig; the rig clamps that pitch into [MinPitch, MaxPitch]. Because the rig runs in
    /// the View phase, the produced camera pose is purely local — never authoritative, never on the
    /// wire.
    ///
    /// Yaw is a heading *relative to the target's forward*, resolved against the target's up each
    /// tick rather than stored as an absolute direction — an absolute forward would need
    /// re-projecting every tick and would drift as the up changed. An entity carrying a
    /// FirstPersonRig is skipped by the plain CameraLook arm, so the two never both write the pose.
    struct FirstPersonRig
    {
        /// @brief The entity the camera looks out of; its up and forward drive the basis.
        Entity Target = Entity::Null;
        /// @brief Eye position offset, in the target's (or the named socket's) local frame.
        vec3 EyeOffset{0.0f, 1.6f, 0.0f};
        /// @brief Optional mesh socket on the target naming the eye anchor; empty uses EyeOffset alone.
        string EyeSocket;
        /// @brief Smallest allowed pitch about the horizon, in radians; negative looks down.
        f32 MinPitch = -1.4f;
        /// @brief Largest allowed pitch about the horizon, in radians; positive looks up.
        f32 MaxPitch = 1.4f;
        /// @brief View-bob amplitude in metres; 0 disables the bob.
        f32 BobAmplitude = 0.0f;
        /// @brief View-bob cycles per metre travelled along the ground plane.
        f32 BobFrequency = 1.0f;
        /// @brief Accumulated bob phase, in radians.
        ///
        /// Runtime view state advanced from the target's planar speed each tick — not authored and
        /// not serialized; it starts at zero on spawn.
        f32 BobPhase = 0.0f;
    };
}

VE_REFLECT(::Veng::CameraFollow, 0xF8BD924F0A0F9DB0ULL)
VE_FIELD(Target, .DisplayName = "Target")
VE_FIELD(Offset, .DisplayName = "Offset")
VE_FIELD(Damping, .DisplayName = "Damping", .Display = {.Min = 0.0})
VE_REFLECT_END();

VE_REFLECT(::Veng::CameraLook, 0x08B30475EB8C5788ULL)
VE_FIELD(Yaw, .DisplayName = "Yaw",
         .Tooltip = "Heading about world up, radians; positive turns left")
VE_FIELD(Pitch, .DisplayName = "Pitch", .Tooltip = "Elevation, radians; positive looks up")
VE_FIELD(PitchLimit, .DisplayName = "Pitch Limit", .Tooltip = "Maximum |Pitch| in radians",
         .Display = {.Min = 0.0})
VE_REFLECT_END();

VE_REFLECT(::Veng::CameraOrbit, 0xE2510C54F8FF9F38ULL)
VE_FIELD(Focus, .DisplayName = "Focus", .Tooltip = "Orbited point, world space")
VE_FIELD(Distance, .DisplayName = "Distance", .Tooltip = "Eye distance from the focus",
         .Display = {.Min = 0.0})
VE_FIELD(MinDistance, .DisplayName = "Min Distance", .Display = {.Min = 0.0})
VE_FIELD(MaxDistance, .DisplayName = "Max Distance", .Display = {.Min = 0.0})
VE_FIELD(Yaw, .DisplayName = "Yaw",
         .Tooltip = "Heading about world up, radians; positive turns left")
VE_FIELD(Pitch, .DisplayName = "Pitch",
         .Tooltip = "Elevation, radians; positive tilts the camera up toward the focus")
VE_FIELD(PitchLimit, .DisplayName = "Pitch Limit", .Tooltip = "Maximum |Pitch| in radians",
         .Display = {.Min = 0.0})
VE_FIELD(FocusTarget, .DisplayName = "Focus Target", .Tooltip = "Point the focus glides toward")
VE_FIELD(FocusDamping, .DisplayName = "Focus Damping",
         .Tooltip = "Exponential-smoothing rate per second; 0 snaps", .Display = {.Min = 0.0})
VE_REFLECT_END();
VE_REFLECT(::Veng::FirstPersonRig, 0xDDD797C278D97169ULL)
VE_FIELD(Target, .DisplayName = "Target")
VE_FIELD(EyeOffset, .DisplayName = "Eye Offset",
         .Tooltip = "Eye position in the target's (or the socket's) local frame")
VE_FIELD(
    EyeSocket, .DisplayName = "Eye Socket",
    .Tooltip = "Optional mesh socket on the target naming the eye anchor; empty uses Eye Offset")
VE_FIELD(MinPitch, .DisplayName = "Min Pitch",
         .Tooltip = "Smallest pitch about the horizon, radians")
VE_FIELD(MaxPitch, .DisplayName = "Max Pitch",
         .Tooltip = "Largest pitch about the horizon, radians")
VE_FIELD(BobAmplitude, .DisplayName = "Bob Amplitude",
         .Tooltip = "View-bob amplitude in metres; 0 disables", .Display = {.Min = 0.0})
VE_FIELD(BobFrequency, .DisplayName = "Bob Frequency",
         .Tooltip = "View-bob cycles per metre travelled", .Display = {.Min = 0.0})
VE_REFLECT_END();

// CameraFollow / CameraLook / CameraOrbit / FirstPersonRig are not VE_REPLICATED: View-phase camera
// rig state, derived locally per client and "never on the wire" (see each struct's doc) — the client
// owns its own camera.
