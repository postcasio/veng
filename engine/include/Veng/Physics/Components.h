#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/CollisionShape.h>
#include <Veng/Physics/Layers.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Scene/Entity.h>

#include <numbers>

namespace Veng
{
    class Scene;

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

    /// @brief How the solver finds a moving body's contacts within one step.
    ///
    /// Integer values are stable — persisted in prefabs.
    enum class MotionQuality : u32
    {
        /// @brief Contacts are found at the pose the step starts from, within a short speculative
        ///        reach, so a body moving further than its own thickness in a step can pass through
        ///        a thin one.
        Discrete = 0,
        /// @brief A body moving far in a step is swept along its path and stopped at the first
        ///        thing it meets, so it does not tunnel.
        ///
        /// A swept body that meets anything is placed at the contact and stays there for the rest
        /// of the collision step, whatever the other body weighs: it loses that collision step's
        /// remaining travel. Against a wall or a heavy body that is the stop; at a glancing touch it
        /// is time stolen, bounded by one collision step's travel (PhysicsWorldInfo::CollisionSteps).
        /// Only a Dynamic body is swept — a Kinematic one is never stopped by what it meets.
        LinearCast = 1,
    };

    /// @brief Default ceiling on a body's linear speed, in metres per second: the solver's own.
    inline constexpr f32 DefaultMaxLinearSpeed = 500.0f;

    /// @brief Default ceiling on a body's angular speed, in radians per second: the solver's own,
    ///        a quarter turn per step at 60 Hz.
    inline constexpr f32 DefaultMaxAngularSpeed = 0.25f * std::numbers::pi_v<f32> * 60.0f;

    /// @brief Default distance a body pair may shift relative to each other before its cached
    ///        contacts are recomputed, in metres: the solver's own, 1 mm.
    inline constexpr f32 DefaultContactCacheMaxDistance = 0.001f;

    /// @brief Default angle a body pair may turn relative to each other before its cached contacts
    ///        are recomputed, in degrees: the solver's own.
    inline constexpr f32 DefaultContactCacheMaxAngle = 2.0f;

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
        /// @brief Mass in kilograms; ignored for a Static body.
        ///
        /// Zero derives the mass from the collider's volume, after its Scale, at the solver's
        /// default density of 1000 kg/m³; any positive value is the mass whatever the shape's size.
        /// A Kinematic body carries it unread, so one turned Dynamic in place weighs what it says.
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
        /// @brief Whether a Dynamic body is created asleep rather than awake.
        ///
        /// A body placed at rest — one of a field of resting bodies — costs the step nothing until
        /// something wakes it: a contact with a moving body, an impulse, or
        /// PhysicsWorld::ActivateBody. Read when the body is created only, so changing it on a
        /// live body re-creates nothing. Ignored for a Static or Kinematic body.
        bool StartAsleep = false;
        /// @brief How the solver finds this body's contacts within a step; ignored for a Static body.
        ///
        /// MotionQuality::LinearCast keeps a fast body from passing through what lies in its path,
        /// at the cost of the time it steals on contact. Changing it on a live body is taken in
        /// place, without re-creating the body.
        MotionQuality Quality = MotionQuality::Discrete;
        /// @brief Ceiling on the body's linear speed in metres per second; must be positive.
        ///
        /// The solver clamps a body moving faster, whatever moved it. Changing it on a live body is
        /// taken in place. Ignored for a Static body.
        f32 MaxLinearSpeed = DefaultMaxLinearSpeed;
        /// @brief Ceiling on the body's angular speed in radians per second; must be positive.
        ///
        /// The solver clamps a body spinning faster, whatever spun it. Changing it on a live body is
        /// taken in place. Ignored for a Static body.
        f32 MaxAngularSpeed = DefaultMaxAngularSpeed;
        /// @brief Multiplier on the inertia derived from the collider's shape; must be positive.
        ///
        /// The derived inertia is that of a uniformly dense solid at the body's mass, authored or
        /// derived. A body whose mass is not spread that way — concentrated near its centre, or
        /// low — says so here: below one a contact or an off-centre impulse turns the body more
        /// readily, above one less. Changing it re-creates the body, as a mass change does.
        /// Ignored for a Static or Kinematic body.
        f32 InertiaScale = 1.0f;
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
        /// @brief Orientation of the shape in the body's local frame; applied before the Offset.
        ///
        /// A primitive is otherwise axis-aligned to the body — a box to its axes, a capsule about
        /// local Y — so this is how one is turned (a capsule laid along the body's forward axis, a
        /// box tilted) with no cooked-geometry detour. It orients cooked geometry too.
        quat Rotation = quat(1.0f, 0.0f, 0.0f, 0.0f);
        /// @brief Uniform scale of the whole collider about the body's origin; must be positive.
        ///
        /// Scales the geometry and its Offset alike, as a uniform scale on the body's local frame
        /// would, so one cooked or adopted shape serves bodies of many sizes. The world builds the
        /// unscaled shape once and wraps it per body, so a thousand bodies at a hundred scales share
        /// one built base shape.
        f32 Scale = 1.0f;
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

    /// @brief Most entries a BodyContacts holds after one step.
    ///
    /// A body touching more bodies than this keeps the ones it approached fastest; the rest are
    /// counted in PhysicsStepStats::ContactsDropped.
    inline constexpr u32 MaxBodyContacts = 16;

    /// @brief Marks an entity whose body's contacts are published after each step.
    ///
    /// Opt-in, so a world that marks nothing records nothing. A marked entity with a body is given a
    /// BodyContacts at the step's reconcile, as a Sensor is given a SensorOverlaps, and the step
    /// writes into it what the body touched — a vehicle's impacts, a body that takes damage from
    /// what it hits, the loudness of an impact sound — for a gameplay system to drain in its own
    /// phase. Adding or removing the marker is taken in place: the body is not re-created, and a
    /// marked body moves exactly as an unmarked one does.
    struct ContactReport
    {
    };

    /// @brief One body another body touched during the last step.
    ///
    /// Every field is in the physics world's frame. A pair touching through several sub-shapes in
    /// one step is one entry: the point and normal of the deepest contact point, the largest
    /// approach speed, and the summed impulse.
    struct BodyContact
    {
        /// @brief The entity owning the body touched.
        Entity Other;
        /// @brief World-space contact point, midway between the two surfaces at the deepest point.
        dvec3 Point{0.0};
        /// @brief Unit contact normal pointing out of Other, toward this body.
        vec3 Normal{0.0f};
        /// @brief How fast the two bodies were closing at Point along Normal, in metres per second,
        ///        before the solver acted on the contact; positive when closing.
        ///
        /// Each body's velocity at the point includes its spin. It includes the velocity gravity
        /// and forces add during the step, so a body resting under gravity reports about one
        /// step's worth of fall: g·Δt divided by the world's collision steps. The largest over the
        /// step's collision steps.
        f32 ApproachSpeed = 0.0f;
        /// @brief The normal impulse the contact is estimated to carry, in newton-seconds, summed
        ///        over the step's collision steps.
        ///
        /// An estimate of the pair in isolation at their combined friction and restitution, not
        /// the impulse the solver applied: the solver's iterations, the other contacts in the same
        /// island, continuous collision and penetration recovery can each differ from it. A
        /// consumer needing the change of velocity the step caused reads the body's velocity after
        /// the step.
        f32 Impulse = 0.0f;
        /// @brief Whether the solver first reported the pair this step, rather than carrying a
        ///        contact over from the previous one.
        bool Began = false;
    };

    /// @brief The bodies an entity's body touched during the last step.
    ///
    /// Written by the step onto every entity carrying a ContactReport and a body: one entry per
    /// other body, at most MaxBodyContacts, ordered fastest approach first. A pair that has
    /// separated is simply absent; a consumer needing an exit compares Current against its own last
    /// read. State a system drains, not an event it subscribes to: nothing a consumer does runs
    /// inside the solver's step.
    ///
    /// @warning The solver finds contacts for the bodies it simulates, so a pair at rest whose two
    /// bodies have both gone to sleep is not reported. Treat the set as "touching and active", as
    /// a sensor's is.
    ///
    /// Runtime-only derived state: it carries no reflected field, so it never serializes and never
    /// rides the wire.
    struct BodyContacts
    {
        /// @brief The bodies touched during the last step, fastest approach first.
        vector<BodyContact> Current;
    };

    /// @brief Marks an entity whose physics state participates in rollback — a predicted body.
    ///
    /// A client that predicts a body's motion ahead of the server re-simulates it during
    /// reconciliation: on a correction it restores the body to the authoritative state and replays
    /// the recorded input forward. This marker declares *which* bodies that applies to, and the set
    /// is deliberately small: a locally-controlled character, a vehicle under the local seat's
    /// controller, and any loose body that must roll back with it while it matters — one the vehicle
    /// is touching, say. A marked rigid body's solver state is saved after every step and restored
    /// at a rollback (BeginPhysicsReplay). Everything else is **static**, which never changes;
    /// **deterministic from the tick** — a kinematic platform or a spinning body, whose pose a Sim
    /// system recomputes from the tick number and which is therefore *re-driven* during a replay
    /// rather than stored; or **held**: a body nobody predicts is put back after the replay exactly
    /// as it stood before it, so it advances by live ticks only. So the state saved per predicted
    /// tick is the predicted set's, not the whole world's.
    ///
    /// @warning Getting the set wrong is a desync that surfaces only when a particular body happens
    /// to matter to the outcome, so the rule is **declared here, not discovered**: mark a body
    /// Predicted only when this peer re-simulates it (its own character or vehicle, or a body that
    /// vehicle strikes), never a body a deterministic Sim system already reproduces from the tick. A
    /// held body a predicted one strikes during a replay is struck where it stands now, not where it
    /// stood at the replayed tick, and the strike is discarded; a predicted character left unmarked
    /// would not be re-seated on a correction and would drift from the server irrecoverably.
    ///
    /// Runtime-only: it carries no reflected field, so it never serializes and never rides the wire.
    /// It is each client's own local stance toward a body it predicts, added on possession and
    /// dropped when possession changes — never authored, never persisted.
    struct Predicted
    {
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

    /// @brief One authored override of a layer pair in a PhysicsSettings collision table.
    ///
    /// A rule sets the pair in both directions, so a table built from rules is symmetric by
    /// construction and the asymmetric-matrix assert in PhysicsWorld::Create cannot be reached from
    /// authored data. Naming a layer against itself sets that one entry.
    struct PhysicsCollisionRule
    {
        /// @brief One layer of the pair.
        PhysicsLayer A = PhysicsLayer::Moving;
        /// @brief The other layer of the pair.
        PhysicsLayer B = PhysicsLayer::Moving;
        /// @brief Whether bodies on the two layers produce contacts.
        bool Collide = true;
    };

    /// @brief The authored settings a scene's PhysicsWorld is created from.
    ///
    /// World-scoped config, found by `Scene::TryGetFirst<PhysicsSettings>()`: one is expected per
    /// scene, and with several the first wins. PhysicsSystem creates the scene's world from it at
    /// start when no world is installed yet (EnsurePhysicsWorld), and a scene carrying none gets a
    /// default world, so a level that names PhysicsSystem simulates with no code at all. The fields
    /// are read once, when the world is created; editing them on a running world changes nothing.
    ///
    /// @warning A joining client spawns the world prefab without its server-authoritative entities,
    /// so a PhysicsSettings on a Server-tier entity (the tier an entity without an Authority has) is
    /// absent on the client, which then builds a default world. Author it on a `Tier::Local` entity
    /// so every peer creates the same world.
    struct PhysicsSettings
    {
        /// @brief Uniform gravity in metres per second squared, in force while the scene carries no
        ///        GravitySource.
        vec3 Gravity = vec3(0.0f, -9.81f, 0.0f);
        /// @brief Overrides applied in order over DefaultCollisionMatrix(); a later rule naming the
        ///        same pair wins.
        vector<PhysicsCollisionRule> Collisions;
        /// @brief Hard ceiling on simultaneously live bodies; zero falls back to the default.
        ///
        /// Exceeding it is fatal, as PhysicsWorldInfo::MaxBodies explains, so size it for the most
        /// bodies the scene can hold at once with headroom.
        u32 MaxBodies = 4096;
        /// @brief Hard ceiling on broad-phase body pairs tracked in one step; zero falls back to
        ///        the default.
        u32 MaxBodyPairs = 16384;
        /// @brief Hard ceiling on contact constraints solved in one step; zero falls back to the
        ///        default.
        u32 MaxContactConstraints = 8192;
        /// @brief Whether the created world starts with its debug draw enabled.
        bool DebugDraw = false;
        /// @brief Collision steps the solver runs inside each world step; zero falls back to one.
        ///
        /// See PhysicsWorldInfo::CollisionSteps.
        u32 CollisionSteps = 1;
        /// @brief Whether a body pair that has barely moved relative to itself reuses its last
        ///        contacts. See PhysicsWorldInfo::ContactCacheEnabled.
        bool ContactCacheEnabled = true;
        /// @brief Relative shift, in metres, past which a pair's cached contacts are recomputed;
        ///        a negative or non-finite value falls back to the default.
        ///
        /// See PhysicsWorldInfo::ContactCacheMaxDistance.
        f32 ContactCacheMaxDistance = DefaultContactCacheMaxDistance;
        /// @brief Relative turn, in degrees, past which a pair's cached contacts are recomputed; a
        ///        value outside [0, 180] falls back to the default.
        ///
        /// See PhysicsWorldInfo::ContactCacheMaxAngle.
        f32 ContactCacheMaxAngle = DefaultContactCacheMaxAngle;
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

namespace Veng::Detail
{
    /// @brief Tells @p scene's physics world that a physics component on @p entity is going.
    ///
    /// The removal hook RigidBody, Collider, Sensor and ContactReport share: it queues the entity
    /// for the next
    /// step's reconcile, which destroys or re-creates its body, so the step finds what left
    /// without sweeping every body. A no-op on a scene with no physics world.
    /// @param scene   The scene the component is leaving.
    /// @param entity  The entity losing it.
    VE_API void NotePhysicsComponentRemoved(const Scene& scene, Entity entity);
}

VE_ENUM(::Veng::MotionType, 0x1C3072A489F0AFDFULL)
VE_ENUMERATOR(Static)
VE_ENUMERATOR(Kinematic)
VE_ENUMERATOR(Dynamic)
VE_ENUM_END();

VE_ENUM(::Veng::MotionQuality, 0xF95BA9A20A617CF7ULL)
VE_ENUMERATOR(Discrete)
VE_ENUMERATOR(LinearCast)
VE_ENUM_END();

VE_ENUM(::Veng::ColliderShape, 0xC35249BF6FEE805EULL)
VE_ENUMERATOR(Box)
VE_ENUMERATOR(Sphere)
VE_ENUMERATOR(Capsule)
VE_ENUMERATOR(Mesh)
VE_ENUM_END();

// A component leaving takes its entity's body with it at the next step's reconcile.
VE_ON_REMOVE(::Veng::RigidBody,
             [](::Veng::Scene& scene, const ::Veng::Entity owner, const ::Veng::RigidBody&)
             { ::Veng::Detail::NotePhysicsComponentRemoved(scene, owner); });

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
VE_FIELD(StartAsleep, .DisplayName = "Start Asleep",
         .Tooltip = "Create a Dynamic body asleep, so it costs nothing until something wakes it")
VE_FIELD(Quality, .DisplayName = "Motion Quality",
         .Tooltip = "Discrete, or LinearCast to sweep a fast body so it does not tunnel")
VE_FIELD(MaxLinearSpeed, .DisplayName = "Max Linear Speed",
         .Tooltip = "Metres per second the solver clamps the body to")
VE_FIELD(MaxAngularSpeed, .DisplayName = "Max Angular Speed",
         .Tooltip = "Radians per second the solver clamps the body to")
VE_FIELD(InertiaScale, .DisplayName = "Inertia Scale",
         .Tooltip = "Multiplier on the shape's inertia; below one a contact turns the body more")
VE_REFLECT_END();

VE_ON_REMOVE(::Veng::Collider,
             [](::Veng::Scene& scene, const ::Veng::Entity owner, const ::Veng::Collider&)
             { ::Veng::Detail::NotePhysicsComponentRemoved(scene, owner); });

VE_REFLECT(::Veng::Collider, 0xA0F0111F12C0F380ULL)
VE_FIELD(Shape, .DisplayName = "Shape")
VE_FIELD(Extents, .DisplayName = "Extents",
         .Tooltip = "Box half sizes; sphere radius in x; capsule radius in x, half height in y")
VE_FIELD(Offset, .DisplayName = "Offset", .Tooltip = "Shape centre in the body's local frame")
VE_FIELD(Rotation, .DisplayName = "Rotation",
         .Tooltip = "Shape orientation in the body's local frame (turns a primitive)")
VE_FIELD(Scale, .DisplayName = "Scale",
         .Tooltip = "Uniform scale of the shape and its offset about the body's origin")
VE_FIELD(Friction, .DisplayName = "Friction")
VE_FIELD(Restitution, .DisplayName = "Restitution", .Tooltip = "Bounciness in [0, 1]")
VE_FIELD(Geometry, .DisplayName = "Geometry",
         .Tooltip = "Cooked collision geometry; used when Shape is Mesh")
VE_REFLECT_END();

VE_TYPE(::Veng::PhysicsPose, 0x48C7721C19164B98ULL);

VE_TYPE(::Veng::Predicted, 0x8DE09914FAA9C0BEULL);

VE_ON_REMOVE(::Veng::Sensor,
             [](::Veng::Scene& scene, const ::Veng::Entity owner, const ::Veng::Sensor&)
             { ::Veng::Detail::NotePhysicsComponentRemoved(scene, owner); });

VE_REFLECT(::Veng::Sensor, 0x4279DEE7A65B8C9BULL)
VE_FIELD(Layers, .DisplayName = "Layers",
         .Tooltip = "Bitmask of PhysicsLayers whose overlaps are published")
VE_REFLECT_END();

VE_TYPE(::Veng::SensorOverlaps, 0x1D7CBF1FE92C5F71ULL);

VE_ON_REMOVE(::Veng::ContactReport,
             [](::Veng::Scene& scene, const ::Veng::Entity owner, const ::Veng::ContactReport&)
             { ::Veng::Detail::NotePhysicsComponentRemoved(scene, owner); });

VE_REFLECT(::Veng::ContactReport, 0xB60655B1FAD46EFBULL)
VE_REFLECT_END();

VE_TYPE(::Veng::BodyContacts, 0xD37840E8065DE7E9ULL);

VE_REFLECT(::Veng::FixedConstraint, 0x24E1EB68FD0AD48FULL)
VE_FIELD(Target, .DisplayName = "Target", .Tooltip = "The other body this one is welded to")
VE_REFLECT_END();

VE_REFLECT(::Veng::PointConstraint, 0x23223F4E1DFEA693ULL)
VE_FIELD(Target, .DisplayName = "Target", .Tooltip = "The other body this one is pinned to")
VE_FIELD(Point, .DisplayName = "Point", .Tooltip = "Shared pivot, in the physics world's frame")
VE_REFLECT_END();

VE_REFLECT(::Veng::PhysicsCollisionRule, 0x1DF51D45AEF4B94DULL)
VE_FIELD(A, .DisplayName = "Layer A")
VE_FIELD(B, .DisplayName = "Layer B")
VE_FIELD(Collide, .DisplayName = "Collide", .Tooltip = "Whether the pair produces contacts")
VE_REFLECT_END();

VE_REFLECT(::Veng::PhysicsSettings, 0x140F01E7127BE541ULL)
VE_FIELD(Gravity, .DisplayName = "Gravity",
         .Tooltip = "Uniform gravity in m/s^2, in force while no GravitySource is authored")
VE_ARRAY_FIELD(Collisions, .DisplayName = "Collisions",
               .Tooltip = "Layer-pair overrides over the default matrix, each set both ways")
VE_FIELD(MaxBodies, .DisplayName = "Max Bodies", .Tooltip = "Live body ceiling; 0 uses the default")
VE_FIELD(MaxBodyPairs, .DisplayName = "Max Body Pairs",
         .Tooltip = "Broad-phase pair ceiling per step; 0 uses the default")
VE_FIELD(MaxContactConstraints, .DisplayName = "Max Contact Constraints",
         .Tooltip = "Contact constraint ceiling per step; 0 uses the default")
VE_FIELD(DebugDraw, .DisplayName = "Debug Draw",
         .Tooltip = "Start the world with its debug visualization on")
VE_FIELD(CollisionSteps, .DisplayName = "Collision Steps",
         .Tooltip = "Collision steps per world step; 0 uses one")
VE_FIELD(ContactCacheEnabled, .DisplayName = "Contact Cache",
         .Tooltip = "Reuse a body pair's contacts while it barely moves relative to itself")
VE_FIELD(ContactCacheMaxDistance, .DisplayName = "Contact Cache Distance",
         .Tooltip = "Relative shift in metres past which a pair's contacts are recomputed")
VE_FIELD(ContactCacheMaxAngle, .DisplayName = "Contact Cache Angle",
         .Tooltip = "Relative turn in degrees past which a pair's contacts are recomputed; lower "
                    "it for long bodies turning beside resting ones")
VE_REFLECT_END();

VE_REFLECT(::Veng::HingeConstraint, 0x9AB6756E19FC0BCAULL)
VE_FIELD(Target, .DisplayName = "Target", .Tooltip = "The other body this one hinges against")
VE_FIELD(Point, .DisplayName = "Point", .Tooltip = "Point the hinge axis passes through")
VE_FIELD(Axis, .DisplayName = "Axis", .Tooltip = "Hinge axis, in the physics world's frame")
VE_REFLECT_END();
