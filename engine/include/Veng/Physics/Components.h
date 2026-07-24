#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/CollisionShape.h>
#include <Veng/Physics/Layers.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Scene/Entity.h>

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

    /// @brief The shape a Collider describes.
    ///
    /// The first three are primitives that need no cooked data — their whole definition is the
    /// Collider's Extents. Integer values are stable — persisted in prefabs.
    enum class ColliderShape : u32
    {
        /// @brief An axis-aligned box in the body's local frame; Extents are its half sizes.
        Box = 0,
        /// @brief A sphere; Extents.x is the radius.
        Sphere = 1,
        /// @brief A capsule about the body's local Y axis; Extents.x is the radius, Extents.y the
        ///        half height of the cylindrical section.
        Capsule = 2,
        /// @brief Cooked geometry: the Collider's Geometry handle supplies the shape, not Extents.
        ///
        /// The referenced CollisionShape decides whether the shape is a convex hull or a triangle
        /// mesh; a triangle mesh may back a Static or Kinematic body only.
        Mesh = 3,
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

    /// @brief The shape a RigidBody collides with, in the body's local frame.
    ///
    /// The three primitives need no cooked data, so a Collider describing one is complete on its
    /// own; ColliderShape::Mesh instead takes its geometry from the cooked CollisionShape the
    /// Geometry handle names. An entity carries at most one Collider.
    struct Collider
    {
        /// @brief Which shape this collider describes.
        ColliderShape Shape = ColliderShape::Box;
        /// @brief The primitive's dimensions, read per Shape (box half sizes; sphere radius in x;
        ///        capsule radius in x and half height in y); ignored under ColliderShape::Mesh.
        vec3 Extents = vec3(0.5f);
        /// @brief Offset of the shape's centre from the body's origin, in the body's local frame.
        vec3 Offset = vec3(0.0f);
        /// @brief Coulomb friction coefficient; the contact combines the two bodies' values.
        f32 Friction = 0.2f;
        /// @brief Bounciness in [0, 1]; 0 is a fully damped contact.
        f32 Restitution = 0.0f;
        /// @brief Cooked geometry backing a ColliderShape::Mesh collider; unused by the primitives.
        ///
        /// Resolved as an ordinary load-time dependency, so a prefab naming one has it resident
        /// before the entity spawns. A ColliderShape::Mesh collider whose handle is not resident
        /// has no shape and its entity is skipped, exactly as a RigidBody with no Collider is.
        AssetHandle<CollisionShape> Geometry;
    };

    /// @brief Makes an entity's body a sensor: it detects overlap and resolves no contact.
    ///
    /// A sensor is still an ordinary body — it has a Collider, sits on a layer, and is filtered by
    /// the world's CollisionMatrix — it simply never pushes and is never pushed. The entities
    /// currently inside it, and the frame's enter/exit deltas, are published on the same entity as
    /// a SensorOverlaps for a gameplay system to drain in its own phase; no game code runs inside
    /// the solver's step.
    ///
    /// A body on PhysicsLayer::Trigger is a sensor whether or not it carries this component — the
    /// layer means exactly that — but only an entity carrying a Sensor is given a SensorOverlaps.
    struct Sensor
    {
        /// @brief Which layers this sensor reports overlaps for; a bit per PhysicsLayer.
        ///
        /// Filters the published overlap set on top of the world's CollisionMatrix, which decides
        /// what the sensor is told about at all. Defaults to every layer.
        u32 Layers = ~0u;
    };

    /// @brief The entities overlapping a Sensor this tick, and the frame's deltas.
    ///
    /// Written by the step onto every Sensor entity. State a system drains, not an event it
    /// subscribes to: a gameplay system reads it in its own phase, so nothing it does runs inside
    /// the solver's step.
    ///
    /// @warning A solver reports contacts for bodies it is simulating, so a dynamic body that has
    /// gone to sleep inside a sensor stops appearing in Current — and is reported in Exited when
    /// it sleeps. Treat the set as "overlapping and active", not "overlapping".
    ///
    /// Runtime-only derived state: it carries no reflected field, so it never serializes and never
    /// rides the wire.
    struct SensorOverlaps
    {
        /// @brief Entities overlapping the sensor at the end of this tick, in ascending slot order.
        vector<Entity> Current;
        /// @brief Entities in Current that were not there at the end of the previous tick.
        vector<Entity> Entered;
        /// @brief Entities that were in Current at the end of the previous tick and are not now.
        vector<Entity> Exited;
    };

    /// @brief Welds this entity's body to another's, holding their relative pose.
    ///
    /// The robust way to carry a dynamic body on a moving kinematic one, where friction on a
    /// fast-moving surface is not. The relative pose is captured when the constraint is created,
    /// so the two bodies are latched wherever they stand at that moment.
    ///
    /// A constraint solves for velocity on bodies the solver integrates, so it has **no effect
    /// between two non-dynamic bodies** — at least one of the pair must be Dynamic. Parenting is
    /// how one kinematic body carries another.
    struct FixedConstraint
    {
        /// @brief The other body in the pair; the constraint is inert while it has no body.
        Entity Target;
    };

    /// @brief Pins this entity's body to another's at a shared world-space point.
    ///
    /// A ball joint: the two bodies keep the point coincident and rotate freely about it.
    struct PointConstraint
    {
        /// @brief The other body in the pair; the constraint is inert while it has no body.
        Entity Target;
        /// @brief The shared pivot, in the physics world's frame, at the moment of creation.
        vec3 Point = vec3(0.0f);
    };

    /// @brief Hinges this entity's body to another's about a shared axis.
    ///
    /// One rotational degree of freedom about Axis through Point — a door, a lid, a wheel.
    struct HingeConstraint
    {
        /// @brief The other body in the pair; the constraint is inert while it has no body.
        Entity Target;
        /// @brief The point the hinge axis passes through, in the physics world's frame.
        vec3 Point = vec3(0.0f);
        /// @brief The hinge axis, in the physics world's frame; normalized on creation.
        vec3 Axis = vec3(0.0f, 1.0f, 0.0f);
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
VE_ENUMERATOR(Mesh)
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
VE_FIELD(Geometry, .DisplayName = "Geometry",
         .Tooltip = "Cooked collision geometry; used when Shape is Mesh")
VE_REFLECT_END();

VE_TYPE(::Veng::PhysicsPose, 0x48C7721C19164B98ULL);

VE_REFLECT(::Veng::Sensor, 0x4279DEE7A65B8C9BULL)
VE_FIELD(Layers, .DisplayName = "Layers",
         .Tooltip = "Bitmask of PhysicsLayers whose overlaps are published")
VE_REFLECT_END();

VE_TYPE(::Veng::SensorOverlaps, 0x1D7CBF1FE92C5F71ULL);

VE_REFLECT(::Veng::FixedConstraint, 0x24E1EB68FD0AD48FULL)
VE_FIELD(Target, .DisplayName = "Target", .Tooltip = "The other body this one is welded to")
VE_REFLECT_END();

VE_REFLECT(::Veng::PointConstraint, 0x23223F4E1DFEA693ULL)
VE_FIELD(Target, .DisplayName = "Target", .Tooltip = "The other body this one is pinned to")
VE_FIELD(Point, .DisplayName = "Point", .Tooltip = "Shared pivot, in the physics world's frame")
VE_REFLECT_END();

VE_REFLECT(::Veng::HingeConstraint, 0x9AB6756E19FC0BCAULL)
VE_FIELD(Target, .DisplayName = "Target", .Tooltip = "The other body this one hinges against")
VE_FIELD(Point, .DisplayName = "Point", .Tooltip = "Point the hinge axis passes through")
VE_FIELD(Axis, .DisplayName = "Axis", .Tooltip = "Hinge axis, in the physics world's frame")
VE_REFLECT_END();
