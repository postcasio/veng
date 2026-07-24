#pragma once

#include <Veng/Veng.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Scene/Entity.h>

namespace Veng
{
    /// @brief Action bits a CharacterMovementSystem reads out of Intent::Actions.
    ///
    /// Intent's action bitset is game policy in general, but the builtin character mover assigns
    /// these two bits a fixed meaning so a control system driving a builtin character knows which
    /// bit to raise. A game with its own character mover is free to interpret the bitset otherwise.
    enum class CharacterAction : u32
    {
        /// @brief Requests a jump this tick; consumed only while grounded.
        Jump = 1u << 0,
        /// @brief Selects RunSpeed over WalkSpeed while held.
        Run = 1u << 1,
    };

    /// @brief A walking character's authored shape and feel — a kinematic capsule driven by Intent.
    ///
    /// The component is the authority and a backing kinematic capsule is its shadow: adding one to
    /// an entity creates the capsule on the next character tick, removing it destroys the capsule,
    /// and editing a field re-creates it with the new settings. The capsule owns its position and
    /// asks the world what it hit, which is why it composes with a hand-rolled feel rather than
    /// fighting a solver for it.
    ///
    /// The capsule's *up* is not a world constant: it is re-read every tick from the gravity field
    /// at the character's position (the same evaluator a dynamic body integrates against), so a
    /// character walking around a curved habitat changes its up continuously and one standing on a
    /// rotating surface changes it slowly forever. A character reached by no gravity source keeps
    /// its last up and floats — free-fall is a defined state, not a division by zero.
    struct CharacterController
    {
        /// @brief Capsule radius, in metres.
        f32 Radius = 0.3f;
        /// @brief Capsule height from the ground to the crown, in metres; must exceed twice Radius.
        f32 Height = 1.8f;
        /// @brief Largest obstacle height stepped over rather than blocked by, in metres.
        f32 StepHeight = 0.3f;
        /// @brief Ground steeper than this counts as a wall, not a floor, in radians.
        f32 MaxSlopeAngle = 0.7853982f;
        /// @brief Ground move speed while walking, in metres per second.
        f32 WalkSpeed = 4.0f;
        /// @brief Ground move speed while the run action is held, in metres per second.
        f32 RunSpeed = 7.0f;
        /// @brief Upward speed imparted by a jump, along the current up, in metres per second.
        f32 JumpImpulse = 5.0f;
        /// @brief Horizontal authority while airborne: 0 is ballistic, 1 is full ground control.
        f32 AirControl = 0.2f;
    };

    /// @brief A character's resolved motion state this tick — the output everything else reads.
    ///
    /// Written by the character mover onto every CharacterController entity each tick, so gameplay
    /// reads a character's ground state, up and speed here rather than querying the physics world
    /// again. GroundEntity is what makes "am I standing on something that is moving" answerable
    /// without a second query.
    ///
    /// Runtime-only derived state: it carries no reflected field, so it never serializes and never
    /// rides the wire.
    struct CharacterState
    {
        /// @brief Whether the character is standing on ground it can move on this tick.
        bool Grounded = false;
        /// @brief World-space normal of the ground under the character; zero when airborne.
        vec3 GroundNormal = vec3(0.0f);
        /// @brief The up resolved for the character this tick — the negated, normalized gravity.
        vec3 Up = vec3(0.0f, 1.0f, 0.0f);
        /// @brief Speed along the ground plane (perpendicular to Up), in metres per second.
        f32 PlanarSpeed = 0.0f;
        /// @brief Signed speed along Up, in metres per second; negative while falling.
        f32 VerticalSpeed = 0.0f;
        /// @brief Seconds since the character was last grounded; zero while grounded.
        f32 AirTime = 0.0f;
        /// @brief The entity being stood on, or Entity::Null when airborne.
        Entity GroundEntity = Entity::Null;
    };
}

VE_REFLECT(::Veng::CharacterController, 0xAB503ACCCB880A0AULL)
VE_FIELD(Radius, .DisplayName = "Radius", .Tooltip = "Capsule radius in metres",
         .Display = {.Min = 0.0})
VE_FIELD(Height, .DisplayName = "Height",
         .Tooltip = "Capsule height, ground to crown; must exceed twice the radius",
         .Display = {.Min = 0.0})
VE_FIELD(StepHeight, .DisplayName = "Step Height",
         .Tooltip = "Largest obstacle stepped over rather than blocked by", .Display = {.Min = 0.0})
VE_FIELD(MaxSlopeAngle, .DisplayName = "Max Slope Angle",
         .Tooltip = "Radians; ground steeper than this counts as a wall", .Display = {.Min = 0.0})
VE_FIELD(WalkSpeed, .DisplayName = "Walk Speed", .Display = {.Min = 0.0})
VE_FIELD(RunSpeed, .DisplayName = "Run Speed", .Display = {.Min = 0.0})
VE_FIELD(JumpImpulse, .DisplayName = "Jump Impulse", .Display = {.Min = 0.0})
VE_FIELD(AirControl, .DisplayName = "Air Control",
         .Tooltip = "0 is ballistic, 1 is full ground control while airborne",
         .Display = {.Min = 0.0, .Max = 1.0})
VE_REFLECT_END();

VE_TYPE(::Veng::CharacterState, 0x25A2F102E492E78CULL);
