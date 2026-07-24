#pragma once

#include <Veng/Veng.h>
#include <Veng/Physics/Layers.h>
#include <Veng/Reflection/Reflect.h>

namespace Veng
{
    /// @brief How the solver treats a RigidBody: fixed, driven, or integrated.
    ///
    /// Integer values are stable — persisted in prefabs.
    enum class MotionType : u32
    {
        /// @brief Never moves. The cheapest body; the solver stores it in the non-moving broad phase.
        Static = 0,
        /// @brief Moved by its owner, not by forces. It pushes dynamic bodies and is pushed by nothing.
        Kinematic = 1,
        /// @brief Integrated by the solver under gravity, contacts and damping.
        Dynamic = 2,
    };

    /// @brief The primitive a Collider describes.
    ///
    /// These are the shapes that need no cooked data — their whole definition is the Collider's
    /// Extents. Integer values are stable — persisted in prefabs.
    enum class ColliderShape : u32
    {
        /// @brief An axis-aligned box in the body's local frame; Extents are its half sizes.
        Box = 0,
        /// @brief A sphere; Extents.x is the radius.
        Sphere = 1,
        /// @brief A capsule about the body's local Y axis; Extents.x is the radius, Extents.y the
        ///        half height of the cylindrical section.
        Capsule = 2,
    };

    /// @brief Marks an entity the physics world simulates, and how.
    ///
    /// The component is the authority and the body is its shadow: adding one to an entity that
    /// also carries a Collider creates the backing body on the next step, removing it destroys
    /// the body, and editing a field re-creates the body with the new settings. An entity with a
    /// RigidBody and no Collider has no shape and is ignored.
    ///
    /// @warning Two writers on one Transform fight, and the last one each frame wins. A consumer
    /// whose Transform is driven by its own per-frame pass must not also carry a Dynamic
    /// RigidBody — Kinematic is the mode for *I own the transform, tell me what I hit*. A
    /// consumer whose Transform is a derived projection rather than a pose at all clears
    /// SyncTransform and works through PhysicsPose instead.
    struct RigidBody
    {
        /// @brief How the solver treats this body.
        MotionType Motion = MotionType::Dynamic;
        /// @brief The collision layer this body sits on; the world's CollisionMatrix filters pairs by it.
        PhysicsLayer Layer = PhysicsLayer::Moving;
        /// @brief Mass in kilograms; ignored for a Static or Kinematic body.
        f32 Mass = 1.0f;
        /// @brief Fraction of linear velocity shed per second; ignored for a non-Dynamic body.
        f32 LinearDamping = 0.05f;
        /// @brief Fraction of angular velocity shed per second; ignored for a non-Dynamic body.
        f32 AngularDamping = 0.05f;
        /// @brief Whether the solver and the entity's Transform stay in step.
        ///
        /// With the flag set (the default) the step reads a Kinematic body's target from the
        /// entity's Transform and writes a Dynamic body's result back to it, so PhysicsPose and
        /// Transform agree and the simple case needs no thought. Cleared, the step neither reads
        /// nor writes Transform: PhysicsPose alone is the solver's interface, the consumer writes
        /// it from whatever it considers authoritative and reads the result back, and the entity's
        /// Transform is left entirely to the consumer's own pass. That is the seam a consumer
        /// whose Transform is a *derived per-frame projection* — a floating-origin renderer, a
        /// re-placed impostor — needs in order to bind the solver to a pose that is not the
        /// Transform.
        bool SyncTransform = true;
    };

    /// @brief The primitive shape a RigidBody collides with, in the body's local frame.
    ///
    /// Convex hulls and triangle meshes are cooked assets; these three primitives need no cooked
    /// data, so a Collider is complete on its own. An entity carries at most one.
    struct Collider
    {
        /// @brief Which primitive the extents describe.
        ColliderShape Shape = ColliderShape::Box;
        /// @brief The primitive's dimensions, read per Shape (box half sizes; sphere radius in x;
        ///        capsule radius in x and half height in y).
        vec3 Extents = vec3(0.5f);
        /// @brief Offset of the shape's centre from the body's origin, in the body's local frame.
        vec3 Offset = vec3(0.0f);
        /// @brief Coulomb friction coefficient; the contact combines the two bodies' values.
        f32 Friction = 0.2f;
        /// @brief Bounciness in [0, 1]; 0 is a fully damped contact.
        f32 Restitution = 0.0f;
    };

    /// @brief The authoritative world-space pose of an entity's physics body.
    ///
    /// The engine keeps one on every entity that has a body, adding it at body creation. It is the
    /// solver's own interface to the entity, in the physics world's frame and at double precision,
    /// so a world whose extent outruns f32 keeps sub-millimetre positions far from the origin.
    ///
    /// With RigidBody::SyncTransform set, the step keeps this and the entity's Transform in step
    /// and a consumer can ignore it. With the flag cleared it is the *only* channel: write it to
    /// place a Static or Kinematic body, read it after the step for a Dynamic one.
    ///
    /// Runtime-only derived state: it carries no reflected field, so it never serializes and never
    /// rides the wire.
    struct PhysicsPose
    {
        /// @brief World-space position in the physics world's frame, in metres.
        dvec3 Position{0.0};
        /// @brief World-space orientation.
        quat Rotation{1.0f, 0.0f, 0.0f, 0.0f};
    };
}

VE_ENUM(::Veng::MotionType, 0x1C3072A489F0AFDFULL)
VE_ENUMERATOR(Static)
VE_ENUMERATOR(Kinematic)
VE_ENUMERATOR(Dynamic)
VE_ENUM_END();

VE_ENUM(::Veng::ColliderShape, 0xC35249BF6FEE805EULL)
VE_ENUMERATOR(Box)
VE_ENUMERATOR(Sphere)
VE_ENUMERATOR(Capsule)
VE_ENUM_END();

VE_REFLECT(::Veng::RigidBody, 0xA943AA8DE96FC0ADULL)
VE_FIELD(Motion, .DisplayName = "Motion", .Tooltip = "Static, Kinematic (owner-driven) or Dynamic")
VE_FIELD(Layer, .DisplayName = "Layer", .Tooltip = "Collision layer the world's matrix filters by")
VE_FIELD(Mass, .DisplayName = "Mass", .Tooltip = "Kilograms; Dynamic bodies only")
VE_FIELD(LinearDamping, .DisplayName = "Linear Damping",
         .Tooltip = "Fraction of linear velocity shed per second")
VE_FIELD(AngularDamping, .DisplayName = "Angular Damping",
         .Tooltip = "Fraction of angular velocity shed per second")
VE_FIELD(SyncTransform, .DisplayName = "Sync Transform",
         .Tooltip = "Bind the solver to the entity's Transform; clear to drive it through "
                    "PhysicsPose alone")
VE_REFLECT_END();

VE_REFLECT(::Veng::Collider, 0xA0F0111F12C0F380ULL)
VE_FIELD(Shape, .DisplayName = "Shape")
VE_FIELD(Extents, .DisplayName = "Extents",
         .Tooltip = "Box half sizes; sphere radius in x; capsule radius in x, half height in y")
VE_FIELD(Offset, .DisplayName = "Offset", .Tooltip = "Shape centre in the body's local frame")
VE_FIELD(Friction, .DisplayName = "Friction")
VE_FIELD(Restitution, .DisplayName = "Restitution", .Tooltip = "Bounciness in [0, 1]")
VE_REFLECT_END();

VE_TYPE(::Veng::PhysicsPose, 0x48C7721C19164B98ULL);
