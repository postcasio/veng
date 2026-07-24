#pragma once

// The solver-facing side of the physics module: the PhysicsWorld::Native definition, the
// engine⇄solver conversions, and the shape builder. Shared by PhysicsWorld.cpp and Queries.cpp,
// and included by nothing outside engine/src/Physics — no public header names a JPH type.

#include <Veng/Physics/Components.h>
#include <Veng/Physics/PhysicsWorld.h>

#include <Jolt/Jolt.h>

#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/Body.h>
#include <Jolt/Physics/Body/BodyID.h>
#include <Jolt/Physics/Body/BodyLockInterface.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/Shape/Shape.h>
#include <Jolt/Physics/Constraints/TwoBodyConstraint.h>
#include <Jolt/Physics/PhysicsStepListener.h>
#include <Jolt/Physics/PhysicsSystem.h>

#include <unordered_map>

namespace Veng::Detail
{
    /// @brief Broad-phase layer holding bodies that never move.
    constexpr JPH::BroadPhaseLayer BroadPhaseNonMoving(0);
    /// @brief Broad-phase layer holding bodies that can move.
    constexpr JPH::BroadPhaseLayer BroadPhaseMoving(1);
    /// @brief Number of broad-phase layers the world declares.
    constexpr u32 BroadPhaseLayerCount = 2;

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

    /// @brief Builds the solver shape a Collider describes, offset into the body's frame.
    ///
    /// A ColliderShape::Mesh collider takes its geometry from the Collider's CollisionShape
    /// handle; a handle that is not resident yields null, which is how the step skips an entity
    /// whose geometry has not arrived. Every other shape is built from the Collider alone and
    /// never yields null (an unbuildable primitive is a fatal assert).
    /// @param collider  The shape to build.
    /// @return The built shape, or null when a ColliderShape::Mesh collider has no resident geometry.
    [[nodiscard]] JPH::RefConst<JPH::Shape> BuildShape(const Collider& collider);

    /// @brief One recorded contact, kept for the debug visualization only.
    struct DebugContact
    {
        /// @brief World-space contact point, narrowed to f32 for drawing.
        vec3 Point;
        /// @brief Contact normal pointing out of the second body.
        vec3 Normal;
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
            return broadPhase == BroadPhaseMoving ? "Moving" : "NonMoving";
        }
#endif

    private:
        /// @brief Which object-layer pairs may collide.
        CollisionMatrix m_Matrix;
    };

    /// @brief Collects contact points for the debug visualization and sensor overlaps for gameplay.
    ///
    /// It observes contacts and never changes settings, so it cannot perturb the simulation.
    /// Debug-contact recording is gated by DebugEnabled so a world with the visualization off pays
    /// nothing beyond the virtual call; sensor overlaps are always collected, since a sensor whose
    /// overlaps went unrecorded would report an empty set rather than a stale one.
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

        /// @brief Records a newly detected contact manifold.
        /// @param body1     The first body in the pair.
        /// @param body2     The second body in the pair.
        /// @param manifold  The manifold's points and normal.
        void OnContactAdded(const JPH::Body& body1, const JPH::Body& body2,
                            const JPH::ContactManifold& manifold, JPH::ContactSettings&) override
        {
            Record(body1, body2, manifold);
        }

        /// @brief Records a contact manifold that persists from the previous step.
        /// @param body1     The first body in the pair.
        /// @param body2     The second body in the pair.
        /// @param manifold  The manifold's points and normal.
        void OnContactPersisted(const JPH::Body& body1, const JPH::Body& body2,
                                const JPH::ContactManifold& manifold,
                                JPH::ContactSettings&) override
        {
            Record(body1, body2, manifold);
        }

    private:
        /// @brief Records @p manifold's points and, when either body is a sensor, the overlap.
        /// @param body1     The first body in the pair.
        /// @param body2     The second body in the pair.
        /// @param manifold  The manifold to record.
        void Record(const JPH::Body& body1, const JPH::Body& body2,
                    const JPH::ContactManifold& manifold)
        {
            if (body1.IsSensor() || body2.IsSensor())
            {
                RecordOverlap(body1, body2);
                RecordOverlap(body2, body1);
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

    /// @brief One live body: its solver handle and the component values it was built from.
    struct BodyRecord
    {
        /// @brief The solver's handle for this body.
        JPH::BodyID Id;
        /// @brief The RigidBody settings the body was built from.
        RigidBody Body;
        /// @brief The Collider settings the shape was built from.
        Collider Shape;
        /// @brief Whether the body was created as a sensor.
        bool Sensor = false;
    };

    /// @brief One live constraint: its solver handle and the settings it was built from.
    struct ConstraintRecord
    {
        /// @brief The solver's constraint, owned by this record and by the solver.
        JPH::Ref<JPH::TwoBodyConstraint> Constraint;
        /// @brief The settings the constraint was built from.
        ConstraintSettings Settings;
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
        /// @brief The installed world-space gravity field, empty when none is set.
        vector<GravitySourceInstance> GravitySources;
        /// @brief The step listener applying the field; registered only while a field is installed.
        Detail::GravityFieldListener GravityListener;
        /// @brief Whether the field listener is currently registered with the solver.
        bool GravityFieldActive = false;
        /// @brief Whether a body was created since the last step, so the broad phase is re-optimized.
        bool BroadPhaseDirty = false;
    };
}
