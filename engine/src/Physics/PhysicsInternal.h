#pragma once

// The solver-facing side of the physics module: the PhysicsWorld::Native definition, the
// engine⇄solver conversions, and the shape builder. Shared by PhysicsWorld.cpp and Queries.cpp,
// and included by nothing outside engine/src/Physics — no public header names a JPH type.

#include <Veng/Asset/AssetHandle.h>
#include <Veng/Physics/Components.h>
#include <Veng/Physics/PhysicsWorld.h>

#include <Jolt/Jolt.h>

#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyActivationListener.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Body/BodyLockInterface.h>
#include <Jolt/Physics/Character/CharacterVirtual.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/EstimateCollisionResponse.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/Constraints/TwoBodyConstraint.h>
#include <Jolt/Physics/PhysicsStepListener.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <array>
#include <limits>
#include <mutex>
#include <unordered_map>

namespace Veng::Detail
{
    /// @brief Broad-phase layer holding bodies that never move.
    constexpr JPH::BroadPhaseLayer BroadPhaseNonMoving(0);
    /// @brief Broad-phase layer holding bodies that can move.
    constexpr JPH::BroadPhaseLayer BroadPhaseMoving(1);
    /// @brief Broad-phase layer holding Debris bodies, apart from the movers.
    ///
    /// The solver rebuilds the changed part of a layer's tree each step a body in it moves, so a
    /// resting field of debris kept in the movers' tree would be re-sorted every step anything moves.
    /// Kept apart, it is touched only when debris itself moves, and a mover — which never collides
    /// with debris — never visits it.
    constexpr JPH::BroadPhaseLayer BroadPhaseDebris(2);
    /// @brief Number of broad-phase layers the world declares.
    constexpr u32 BroadPhaseLayerCount = 3;

    /// @brief Converts an engine world-space position into the solver's real-precision vector.
    [[nodiscard]] inline JPH::RVec3 ToJolt(const dvec3 value)
    {
        return {static_cast<JPH::Real>(value.x), static_cast<JPH::Real>(value.y),
                static_cast<JPH::Real>(value.z)};
    }

    /// @brief Converts an engine direction or velocity into the solver's single-precision vector.
    [[nodiscard]] inline JPH::Vec3 ToJolt(const vec3 value)
    {
        return {value.x, value.y, value.z};
    }

    /// @brief Converts an engine orientation into the solver's quaternion (x, y, z, w order).
    [[nodiscard]] inline JPH::Quat ToJolt(const quat value)
    {
        return {value.x, value.y, value.z, value.w};
    }

    /// @brief Converts a solver real-precision position into an engine world-space position.
    [[nodiscard]] inline dvec3 FromJolt(const JPH::RVec3 value)
    {
        return {static_cast<f64>(value.GetX()), static_cast<f64>(value.GetY()),
                static_cast<f64>(value.GetZ())};
    }

    /// @brief Converts a solver single-precision vector into an engine vector.
    [[nodiscard]] inline vec3 FromJolt(const JPH::Vec3 value)
    {
        return {value.GetX(), value.GetY(), value.GetZ()};
    }

    /// @brief Converts a solver quaternion into an engine orientation.
    [[nodiscard]] inline quat FromJolt(const JPH::Quat value)
    {
        return {value.GetW(), value.GetX(), value.GetY(), value.GetZ()};
    }

    /// @brief Packs an entity into a body's user data, so a solver body names its owner directly.
    [[nodiscard]] inline u64 PackEntity(const Entity entity)
    {
        return (u64{entity.Generation} << 32) | entity.Index;
    }

    /// @brief Unpacks the entity PackEntity stored in a body's user data.
    [[nodiscard]] inline Entity UnpackEntity(const u64 packed)
    {
        return Entity{.Index = static_cast<u32>(packed),
                      .Generation = static_cast<u32>(packed >> 32)};
    }

    /// @brief Builds the solver shape a Collider describes, offset into the body's frame.
    ///
    /// A ColliderShape::Mesh collider takes its geometry from the Collider's CollisionShape
    /// handle; a handle that is not resident yields null, which is how the step skips an entity
    /// whose geometry has not arrived. Every other shape is built from the Collider alone and
    /// never yields null (an unbuildable primitive is a fatal assert).
    ///
    /// Always builds afresh; bodies and queries resolve their shape through the world's ShapeCache,
    /// which calls this only on a miss.
    /// @param collider  The shape to build.
    /// @return The built shape, or null when a ColliderShape::Mesh collider has no resident geometry.
    [[nodiscard]] JPH::RefConst<JPH::Shape> BuildShape(const Collider& collider);

    /// @brief Everything that determines the solver shape a Collider builds into.
    ///
    /// Friction and restitution are body settings, not shape settings, so two colliders differing
    /// only in those share a shape. Floats compare by bit pattern with negative zero folded onto
    /// zero, so equal colliders always meet and a NaN field still finds itself.
    struct ShapeKey
    {
        /// @brief The collider's shape kind.
        ColliderShape Kind = ColliderShape::Box;
        /// @brief Bit patterns of Extents (unused under Mesh, so zeroed), Offset, then Rotation.
        std::array<u32, 10> Bits{};
        /// @brief The cooked geometry's identity under ColliderShape::Mesh; null otherwise.
        const CollisionShape* Geometry = nullptr;

        /// @brief Builds the key for @p collider.
        [[nodiscard]] static ShapeKey Of(const Collider& collider);

        /// @brief Field-wise equality.
        bool operator==(const ShapeKey&) const = default;
    };

    /// @brief Hashes a ShapeKey over every field.
    struct ShapeKeyHash
    {
        /// @brief Returns @p key's hash.
        [[nodiscard]] usize operator()(const ShapeKey& key) const;
    };

    /// @brief The world's built collider shapes, shared by every body and query that names one.
    ///
    /// A compound collider costs a heap-allocated build per child plus a bounding tree, and the
    /// shape it builds is immutable, so building it once per distinct collider and sharing the
    /// result is free of any behavioural change. Entries are reference-counted solver shapes:
    /// a body keeps its entry alive by holding the shape, and Sweep drops an entry nothing has
    /// held or asked for since the previous sweep, so the cache is bounded by the colliders in
    /// use. An entry also holds the cooked geometry's handle, which pins the geometry's address
    /// for as long as the entry keys on it.
    ///
    /// Guarded by a mutex, because a query may run on any thread a caller chooses.
    class ShapeCache
    {
    public:
        /// @brief Returns the built shape for @p collider, building and keeping it on a miss.
        /// @param collider  The shape to resolve.
        /// @param step      The world's current step count, stamping the entry as used.
        /// @return The shared shape, or null when a ColliderShape::Mesh collider has no resident
        ///         geometry (which is not kept, so it is built the tick the geometry arrives).
        [[nodiscard]] JPH::RefConst<JPH::Shape> Acquire(const Collider& collider, u64 step);

        /// @brief Returns the shape held for @p collider without building one, or null.
        [[nodiscard]] const JPH::Shape* Find(const Collider& collider) const;

        /// @brief Drops every entry no body holds and nothing has acquired since the previous step.
        /// @param step  The world's step count before the step that is about to run; an entry
        ///              stamped with it was acquired since the previous step, and is kept.
        void Sweep(u64 step);

        /// @brief Returns how many shapes the cache holds.
        [[nodiscard]] usize Size() const;

    private:
        /// @brief One cached shape.
        struct Entry
        {
            /// @brief The built shape; the cache's own reference is one of its holders.
            JPH::RefConst<JPH::Shape> Shape;
            /// @brief The cooked geometry the shape was built from, held so its address stays put.
            AssetHandle<CollisionShape> Geometry;
            /// @brief The step count at the entry's last acquisition.
            u64 LastUsed = 0;
        };

        /// @brief Serializes every access to the map.
        mutable std::mutex m_Mutex;
        /// @brief The cached shapes, keyed by what built them.
        std::unordered_map<ShapeKey, Entry, ShapeKeyHash> m_Shapes;
    };

    /// @brief One recorded contact, kept for the debug visualization only.
    struct DebugContact
    {
        /// @brief World-space contact point, narrowed to f32 for drawing.
        vec3 Point;
        /// @brief Contact normal pointing out of the first body, toward the second.
        vec3 Normal;
    };

    /// @brief One manifold's contact as one reporting body sees it, before the step merges a pair's
    ///        manifolds into one BodyContact.
    struct ContactSample
    {
        /// @brief The reporting body's entity.
        Entity Self;
        /// @brief Penetration of the manifold's deepest point; negative while still apart.
        f32 Depth = 0.0f;
        /// @brief The contact as Self sees it.
        BodyContact Contact;
    };

    /// @brief The world's layer table: broad-phase mapping plus both collision filters.
    ///
    /// One object holds all three interfaces because they answer one question — which pairs may
    /// touch — off one CollisionMatrix.
    class LayerTable final : public JPH::BroadPhaseLayerInterface,
                             public JPH::ObjectVsBroadPhaseLayerFilter,
                             public JPH::ObjectLayerPairFilter
    {
    public:
        /// @brief Builds the table over a collision matrix.
        /// @param matrix  Which object-layer pairs may collide.
        explicit LayerTable(const CollisionMatrix& matrix) : m_Matrix(matrix) {}

        /// @brief Returns how many broad-phase layers the world declares.
        [[nodiscard]] JPH::uint GetNumBroadPhaseLayers() const override
        {
            return BroadPhaseLayerCount;
        }

        /// @brief Maps an object layer onto its broad-phase layer.
        /// @param layer  The object layer to map.
        /// @return The broad-phase layer it lives in.
        [[nodiscard]] JPH::BroadPhaseLayer
        GetBroadPhaseLayer(const JPH::ObjectLayer layer) const override
        {
            const auto engineLayer = static_cast<PhysicsLayer>(layer);
            if (engineLayer == PhysicsLayer::Debris)
            {
                return BroadPhaseDebris;
            }
            const bool moves =
                engineLayer != PhysicsLayer::Static && engineLayer != PhysicsLayer::Query;
            return moves ? BroadPhaseMoving : BroadPhaseNonMoving;
        }

        /// @brief Whether an object layer may collide with anything in a broad-phase layer.
        /// @param layer       The object layer being tested.
        /// @param broadPhase  The broad-phase layer being tested against.
        /// @return True when at least one object layer inside @p broadPhase collides with @p layer.
        [[nodiscard]] bool ShouldCollide(const JPH::ObjectLayer layer,
                                         const JPH::BroadPhaseLayer broadPhase) const override
        {
            for (u32 other = 0; other < PhysicsLayerCount; ++other)
            {
                const auto otherLayer = static_cast<PhysicsLayer>(other);
                if (GetBroadPhaseLayer(static_cast<JPH::ObjectLayer>(other)) != broadPhase)
                {
                    continue;
                }
                if (LayersCollide(m_Matrix, static_cast<PhysicsLayer>(layer), otherLayer))
                {
                    return true;
                }
            }
            return false;
        }

        /// @brief Whether two object layers may collide.
        /// @param a  One object layer.
        /// @param b  The other object layer.
        /// @return True when the matrix allows the pair.
        [[nodiscard]] bool ShouldCollide(const JPH::ObjectLayer a,
                                         const JPH::ObjectLayer b) const override
        {
            return LayersCollide(m_Matrix, static_cast<PhysicsLayer>(a),
                                 static_cast<PhysicsLayer>(b));
        }

#if defined(JPH_EXTERNAL_PROFILE) || defined(JPH_PROFILE_ENABLED)
        /// @brief Returns a broad-phase layer's name, for the solver's own profiler.
        /// @param broadPhase  The layer to name.
        [[nodiscard]] const char*
        GetBroadPhaseLayerName(const JPH::BroadPhaseLayer broadPhase) const override
        {
            if (broadPhase == BroadPhaseDebris)
            {
                return "Debris";
            }
            return broadPhase == BroadPhaseMoving ? "Moving" : "NonMoving";
        }
#endif

    private:
        /// @brief Which object-layer pairs may collide.
        CollisionMatrix m_Matrix;
    };

    /// @brief Collects contact points for the debug visualization, sensor overlaps, and the contacts
    ///        of the bodies that report them.
    ///
    /// It observes contacts and never changes settings, so it cannot perturb the simulation.
    /// Debug-contact recording is gated by DebugEnabled so a world with the visualization off pays
    /// nothing beyond the virtual call; sensor overlaps are always collected, since a sensor whose
    /// overlaps went unrecorded would report an empty set rather than a stale one. A contact is
    /// sampled only when one of its bodies reports, so a world where none does pays one comparison.
    ///
    /// The solver calls these once per collision step, and once more for a contact its continuous
    /// pass finds, so a pair yields several samples in a step; PhysicsWorld::Step merges them.
    ///
    /// The solver's job system is single-threaded, so these callbacks run on the thread that
    /// called Step and the containers below need no synchronization.
    class ContactRecorder final : public JPH::ContactListener
    {
    public:
        /// @brief Whether contact points are recorded this step.
        bool DebugEnabled = false;
        /// @brief Contacts recorded during the current step.
        vector<DebugContact> Contacts;
        /// @brief Entities overlapping each sensor body this step, keyed by the sensor's entity.
        std::unordered_map<Entity, vector<Entity>> Overlaps;
        /// @brief Entity of each live body, keyed by the body's index-and-sequence value.
        const std::unordered_map<u32, Entity>* BodyOwners = nullptr;
        /// @brief Whether each body reports its contacts, indexed by the body's index; grown on use.
        vector<u8> Reporting;
        /// @brief How many live bodies report their contacts.
        u32 ReportingBodies = 0;
        /// @brief One sample per manifold per reporting body during the current step.
        vector<ContactSample> Samples;
        /// @brief The solver's restitution threshold, which the impulse estimate applies as it does.
        f32 MinVelocityForRestitution = 1.0f;
        /// @brief Iterations the impulse estimate runs: the solver's own velocity steps.
        u32 EstimateIterations = 10;

        /// @brief Whether the body with @p id reports its contacts.
        [[nodiscard]] bool IsReporting(const JPH::BodyID& id) const
        {
            const JPH::uint index = id.GetIndex();
            return index < Reporting.size() && Reporting[index] != 0;
        }

        /// @brief Records a newly detected contact manifold.
        /// @param body1     The first body in the pair.
        /// @param body2     The second body in the pair.
        /// @param manifold  The manifold's points and normal.
        /// @param settings  The pair's combined friction and restitution, read and never written.
        void OnContactAdded(const JPH::Body& body1, const JPH::Body& body2,
                            const JPH::ContactManifold& manifold,
                            JPH::ContactSettings& settings) override
        {
            Record(body1, body2, manifold, settings, true);
        }

        /// @brief Records a contact manifold that persists from the previous step.
        /// @param body1     The first body in the pair.
        /// @param body2     The second body in the pair.
        /// @param manifold  The manifold's points and normal.
        /// @param settings  The pair's combined friction and restitution, read and never written.
        void OnContactPersisted(const JPH::Body& body1, const JPH::Body& body2,
                                const JPH::ContactManifold& manifold,
                                JPH::ContactSettings& settings) override
        {
            Record(body1, body2, manifold, settings, false);
        }

    private:
        /// @brief Records @p manifold's points, the overlap when either body is a sensor, and a
        ///        sample for each body that reports its contacts.
        /// @param body1     The first body in the pair.
        /// @param body2     The second body in the pair.
        /// @param manifold  The manifold to record.
        /// @param settings  The pair's combined contact settings.
        /// @param began     Whether the solver reports the pair as new.
        void Record(const JPH::Body& body1, const JPH::Body& body2,
                    const JPH::ContactManifold& manifold, const JPH::ContactSettings& settings,
                    const bool began)
        {
            if (body1.IsSensor() || body2.IsSensor())
            {
                RecordOverlap(body1, body2);
                RecordOverlap(body2, body1);
            }
            else if (ReportingBodies != 0)
            {
                RecordReport(body1, body2, manifold, settings, began);
            }
            if (!DebugEnabled)
            {
                return;
            }
            const vec3 normal = FromJolt(manifold.mWorldSpaceNormal);
            for (JPH::uint i = 0; i < manifold.mRelativeContactPointsOn1.size(); ++i)
            {
                const dvec3 point = FromJolt(manifold.GetWorldSpaceContactPointOn1(i));
                Contacts.emplace_back(DebugContact{.Point = vec3(point), .Normal = normal});
            }
        }

        /// @brief Records @p other as overlapping @p sensor, when @p sensor really is one.
        /// @param sensor  The candidate sensor body.
        /// @param other   The body touching it.
        void RecordOverlap(const JPH::Body& sensor, const JPH::Body& other)
        {
            if (!sensor.IsSensor() || BodyOwners == nullptr)
            {
                return;
            }
            const auto sensorOwner = BodyOwners->find(sensor.GetID().GetIndexAndSequenceNumber());
            const auto otherOwner = BodyOwners->find(other.GetID().GetIndexAndSequenceNumber());
            if (sensorOwner == BodyOwners->end() || otherOwner == BodyOwners->end())
            {
                return;
            }
            Overlaps[sensorOwner->second].emplace_back(otherOwner->second);
        }

        /// @brief Samples @p manifold for whichever of its two bodies reports its contacts.
        ///
        /// The velocities are read as the solver finds the contact, before it acts on it; the
        /// manifold's normal points out of body 1, toward body 2.
        /// @param body1     The first body in the pair.
        /// @param body2     The second body in the pair.
        /// @param manifold  The manifold to sample.
        /// @param settings  The pair's combined friction and restitution.
        /// @param began     Whether the solver reports the pair as new.
        void RecordReport(const JPH::Body& body1, const JPH::Body& body2,
                          const JPH::ContactManifold& manifold,
                          const JPH::ContactSettings& settings, const bool began)
        {
            const bool reports1 = IsReporting(body1.GetID());
            const bool reports2 = IsReporting(body2.GetID());
            const JPH::uint points = manifold.mRelativeContactPointsOn1.size();
            if ((!reports1 && !reports2) || points == 0)
            {
                return;
            }

            const JPH::Vec3 normal = manifold.mWorldSpaceNormal;
            JPH::uint deepest = 0;
            f32 depth = -std::numeric_limits<f32>::infinity();
            for (JPH::uint i = 0; i < points; ++i)
            {
                const f32 pointDepth =
                    (manifold.mRelativeContactPointsOn1[i] - manifold.mRelativeContactPointsOn2[i])
                        .Dot(normal);
                if (pointDepth > depth)
                {
                    depth = pointDepth;
                    deepest = i;
                }
            }
            // The solver's own convention for a contact point: midway between the two surfaces.
            const JPH::RVec3 point =
                manifold.mBaseOffset + 0.5f * (manifold.mRelativeContactPointsOn1[deepest] +
                                               manifold.mRelativeContactPointsOn2[deepest]);
            const f32 approach =
                (body1.GetPointVelocity(point) - body2.GetPointVelocity(point)).Dot(normal);

            JPH::CollisionEstimationResult estimate;
            JPH::EstimateCollisionResponse(
                body1, body2, manifold, estimate, settings.mCombinedFriction,
                settings.mCombinedRestitution, MinVelocityForRestitution, EstimateIterations);
            f32 impulse = 0.0f;
            for (const f32 contactImpulse : estimate.mContactImpulse)
            {
                impulse += contactImpulse;
            }

            const Entity entity1 = UnpackEntity(body1.GetUserData());
            const Entity entity2 = UnpackEntity(body2.GetUserData());
            const BodyContact contact{
                .Point = FromJolt(point),
                .ApproachSpeed = approach,
                .Impulse = impulse,
                .Began = began,
            };
            if (reports1)
            {
                BodyContact& seen = Samples
                                        .emplace_back(ContactSample{
                                            .Self = entity1, .Depth = depth, .Contact = contact})
                                        .Contact;
                seen.Other = entity2;
                seen.Normal = -FromJolt(normal);
            }
            if (reports2)
            {
                BodyContact& seen = Samples
                                        .emplace_back(ContactSample{
                                            .Self = entity2, .Depth = depth, .Contact = contact})
                                        .Contact;
                seen.Other = entity1;
                seen.Normal = FromJolt(normal);
            }
        }
    };

    /// @brief Applies a per-body gravity field each step, in place of the solver's uniform gravity.
    ///
    /// Registered with the solver only while a field is installed. Its OnStep runs before each
    /// simulation step with every body and constraint locked, so it reads each active dynamic body's
    /// centre of mass, evaluates the field there through EvaluateGravity, and adds the matching
    /// force (mass times acceleration). Static and kinematic bodies do not integrate and are
    /// skipped; a body reached by no source gets no force, which is the free-fall state the field
    /// model makes real.
    ///
    /// The solver's job system is single-threaded, so this runs on the thread that called Step and
    /// the unsafe active-body list it reads is stable for the callback's duration.
    class GravityFieldListener final : public JPH::PhysicsStepListener
    {
    public:
        /// @brief The world-space sources to compose; owned by PhysicsWorld::Native.
        const vector<GravitySourceInstance>* Sources = nullptr;

        /// @brief Adds each active dynamic body its field force for the coming step.
        /// @param context  The step context, carrying the physics system being stepped.
        void OnStep(const JPH::PhysicsStepListenerContext& context) override
        {
            if (Sources == nullptr || Sources->empty())
            {
                return;
            }
            const JPH::PhysicsSystem& system = *context.mPhysicsSystem;
            const JPH::BodyLockInterfaceNoLock& locks = system.GetBodyLockInterfaceNoLock();
            const JPH::uint active = system.GetNumActiveBodies(JPH::EBodyType::RigidBody);
            const JPH::BodyID* bodies = system.GetActiveBodiesUnsafe(JPH::EBodyType::RigidBody);
            for (JPH::uint i = 0; i < active; ++i)
            {
                JPH::Body* body = locks.TryGetBody(bodies[i]);
                if (body == nullptr || !body->IsDynamic())
                {
                    continue;
                }
                const f32 inverseMass = body->GetMotionProperties()->GetInverseMass();
                if (inverseMass <= 0.0f)
                {
                    continue;
                }
                const vec3 position = vec3(FromJolt(body->GetCenterOfMassPosition()));
                const vec3 acceleration = EvaluateGravity(*Sources, position);
                // Force is mass times acceleration; the body's mass is the reciprocal of its
                // inverse mass, so the field applies the same acceleration whatever the body weighs.
                body->AddForce(ToJolt(acceleration / inverseMass));
            }
        }
    };

    /// @brief Records the bodies the solver puts to sleep during a step.
    ///
    /// A body that settles in a step drops out of the active list, so a readback that walked only
    /// the active bodies would never read its final pose. The step reads these beside the active
    /// list. Recording is on only inside PhysicsWorld::Step, so a body deactivated by a removal
    /// between steps is not reported.
    ///
    /// The solver's job system is single-threaded, so the callbacks run on the thread that called
    /// Step and the list needs no synchronization.
    class ActivationRecorder final : public JPH::BodyActivationListener
    {
    public:
        /// @brief Whether deactivations are being recorded.
        bool Recording = false;
        /// @brief The bodies put to sleep during the current step.
        vector<JPH::BodyID> Deactivated;

        /// @brief Ignored: an activated body is in the active list the readback walks.
        void OnBodyActivated(const JPH::BodyID&, JPH::uint64) override {}

        /// @brief Records a body put to sleep while recording.
        /// @param id  The body going to sleep.
        void OnBodyDeactivated(const JPH::BodyID& id, JPH::uint64) override
        {
            if (Recording)
            {
                Deactivated.push_back(id);
            }
        }
    };

    /// @brief Index a BodyRecord holds when its body is Dynamic and so has no DrivenBody.
    constexpr u32 NotDriven = ~0u;

    /// @brief One live body: its solver handle and the component values it was built from.
    struct BodyRecord
    {
        /// @brief The solver's handle for this body.
        JPH::BodyID Id;
        /// @brief The RigidBody settings the body was built from.
        RigidBody Body;
        /// @brief The Collider settings the shape was built from.
        Collider Shape;
        /// @brief Whether the body was created as a sensor (a Sensor component or the Trigger layer).
        bool Sensor = false;
        /// @brief Whether the entity carried a Sensor component when the body was created.
        bool SensorComponent = false;
        /// @brief Whether the body's contacts are recorded each step (the ContactReport marker).
        bool ReportContacts = false;
        /// @brief This body's slot in Native::Driven, or NotDriven for a Dynamic body.
        u32 Driven = NotDriven;
    };

    /// @brief A static or kinematic body the step pushes, packed so the push pass touches no map.
    struct DrivenBody
    {
        /// @brief The entity owning the body.
        Entity Owner;
        /// @brief The solver's handle for the body.
        JPH::BodyID Id;
        /// @brief Static or Kinematic.
        MotionType Motion = MotionType::Static;
        /// @brief Whether the body's target is read from Transform rather than PhysicsPose.
        bool SyncTransform = true;
        /// @brief The pose the body was last placed at, as its source component read then.
        PhysicsPose LastPushed;
        /// @brief Whether a kinematic body was moved toward a new target in the last push, so it
        ///        still carries the velocity that reached it.
        bool Moving = false;
    };

    /// @brief One live constraint: its solver handle and the settings it was built from.
    struct ConstraintRecord
    {
        /// @brief The solver's constraint, owned by this record and by the solver.
        JPH::Ref<JPH::TwoBodyConstraint> Constraint;
        /// @brief The settings the constraint was built from.
        ConstraintSettings Settings;
    };

    /// @brief One live character capsule: the solver object and the controller it was built from.
    struct CharacterRecord
    {
        /// @brief The kinematic capsule, owned by this record.
        JPH::Ref<JPH::CharacterVirtual> Character;
        /// @brief The CharacterController settings the capsule was built from, for idempotent reconcile.
        CharacterController Settings;
    };
}

namespace Veng
{
    struct PhysicsWorld::Native
    {
        /// @brief Builds the backend state from the world's descriptor.
        /// @param info  Gravity, the collision matrix, and the body budget.
        explicit Native(const PhysicsWorldInfo& info);

        /// @brief The broad-phase mapping and both collision filters.
        Detail::LayerTable Layers;
        /// @brief Scratch the solver carves its per-step allocations out of.
        JPH::TempAllocatorImpl Temp;
        /// @brief The solver's job system, run on the calling thread.
        ///
        /// Single-threaded: veng's render thread is single and sharing a pool with the TaskSystem
        /// is a separate design question, so the step runs entirely on its caller.
        JPH::JobSystemSingleThreaded Jobs{JPH::cMaxPhysicsJobs};
        /// @brief The solver itself.
        JPH::PhysicsSystem System;
        /// @brief The contact collector feeding the debug visualization and the sensor overlaps.
        Detail::ContactRecorder Contacts;
        /// @brief Live bodies, keyed by the entity that owns them.
        std::unordered_map<Entity, Detail::BodyRecord> Bodies;
        /// @brief The owning entity of each live body, keyed by its index-and-sequence value.
        std::unordered_map<u32, Entity> BodyOwners;
        /// @brief Live constraints, keyed by the entity carrying the constraint component.
        std::unordered_map<Entity, Detail::ConstraintRecord> Constraints;
        /// @brief Live character capsules, keyed by the entity that owns them.
        std::unordered_map<Entity, Detail::CharacterRecord> Characters;
        /// @brief Built collider shapes, shared by bodies and queries naming equal colliders.
        Detail::ShapeCache Shapes;
        /// @brief The installed world-space gravity field, empty when none is set.
        vector<GravitySourceInstance> GravitySources;
        /// @brief The step listener applying the field; registered only while a field is installed.
        Detail::GravityFieldListener GravityListener;
        /// @brief Whether the field listener is currently registered with the solver.
        bool GravityFieldActive = false;
        /// @brief Bodies created since the last step, which decides whether it re-optimizes the
        ///        broad phase.
        u32 CreatedSinceStep = 0;
        /// @brief Whether the last step re-optimized the broad phase.
        bool OptimizedLastStep = false;
        /// @brief The bodies the solver put to sleep during the last step.
        Detail::ActivationRecorder Activations;
        /// @brief Static and kinematic bodies, the only ones the step pushes.
        vector<Detail::DrivenBody> Driven;
        /// @brief Entities whose body was created for a Sensor component, the ones a step publishes.
        vector<Entity> SensorEntities;
        /// @brief Entities whose body reports its contacts, the ones a step publishes contacts to.
        vector<Entity> ReportEntities;
        /// @brief The last step's contacts, one per reporting body and other body, capped at
        ///        MaxBodyContacts per reporting body; ordered by reporting entity, then fastest
        ///        approach first.
        vector<Detail::ContactSample> ReportedContacts;
        /// @brief Contacts the last step's cap left out of a body's report.
        u32 DroppedContacts = 0;
        /// @brief Entities the next reconcile re-checks whatever their change ticks say: a physics
        ///        component left them, or the body API was called on them directly.
        vector<Entity> Pending;
        /// @brief Entities whose ColliderShape::Mesh geometry is not resident yet, re-checked each
        ///        step until it arrives.
        vector<Entity> AwaitingShape;
        /// @brief The scene change tick the last reconcile ran at; a component stamped at or after
        ///        it is a change the next reconcile picks up.
        u64 ReconciledTick = 0;
        /// @brief Whether the next reconcile walks every physics entity, as after a replay or a
        ///        DestroyAllBodies, when change ticks cannot say what moved.
        bool FullReconcile = true;
        /// @brief Whether StepPhysics is reconciling, so its own body calls queue nothing.
        bool Reconciling = false;
        /// @brief Entities whose body was placed or restored outside a step, read back by the next
        ///        step whether or not they are awake.
        vector<Entity> ReadBackNext;
        /// @brief Whether the next step reads every body back, as after a whole-world restore.
        bool FullReadBack = false;
        /// @brief What the last StepPhysics did.
        PhysicsStepStats Stats;
    };
}
