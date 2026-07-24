#include <Veng/Physics/PhysicsWorld.h>

#include <Veng/Assert.h>
#include <Veng/Asset/CollisionShape.h>
#include <Veng/Log.h>
#include <Veng/Renderer/DebugDraw.h>

#include "PhysicsInternal.h"

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/IssueReporting.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Body/BodyLock.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/ConvexHullShape.h>
#include <Jolt/Physics/Collision/Shape/MeshShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/Constraints/FixedConstraint.h>
#include <Jolt/Physics/Constraints/HingeConstraint.h>
#include <Jolt/Physics/Constraints/PointConstraint.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/StateRecorderImpl.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>

namespace Veng
{
    namespace
    {
        /// @brief Floor on the per-step scratch, so a tiny world still has room to solve.
        constexpr u32 MinTempAllocatorBytes = 8U * 1024U * 1024U;

        /// @brief Per-step scratch reserved per budgeted body.
        ///
        /// The solver's island and contact buffers scale with the body budget, and it treats
        /// exhausting the scratch as fatal rather than degrading — so the reservation is derived
        /// from PhysicsWorldInfo::MaxBodies rather than fixed.
        constexpr u32 TempAllocatorBytesPerBody = 4096;

        /// @brief The per-step scratch a body budget needs.
        /// @param maxBodies  The world's body budget.
        /// @return Scratch size in bytes.
        [[nodiscard]] u32 TempAllocatorBytes(const u32 maxBodies)
        {
            return std::max(MinTempAllocatorBytes, maxBodies * TempAllocatorBytesPerBody);
        }

        /// @brief Body mutex count Jolt sizes from the hardware when handed zero.
        constexpr u32 AutoBodyMutexCount = 0;

        /// @brief Collision substeps per Step call; one, because the Sim tick is already fixed.
        constexpr int CollisionStepsPerUpdate = 1;

        /// @brief Routes a solver trace line into the engine log.
        /// @param format  printf-style format string.
        void TraceToLog(const char* format, ...)
        {
            va_list args;
            va_start(args, format);
            char buffer[1024];
            std::vsnprintf(buffer, sizeof(buffer), format, args);
            va_end(args);
            Log::Info("Physics: {}", buffer);
        }

#ifdef JPH_ENABLE_ASSERTS
        /// @brief Routes a solver assertion failure into VE_ASSERT, so it breaks like an engine assert.
        /// @param expression  The failed expression's source text.
        /// @param message     The solver's message, or null.
        /// @param file        Source file of the failure.
        /// @param line        Source line of the failure.
        /// @return False, so the solver does not additionally break on its own.
        bool AssertToVeAssert(const char* expression, const char* message, const char* file,
                              const JPH::uint line)
        {
            VE_ASSERT(false, "Physics assertion failed: {} ({}) at {}:{}", expression,
                      message != nullptr ? message : "", file, line);
            return false;
        }
#endif

        /// @brief Guards the process-wide solver registration below.
        std::mutex g_RegistrationMutex;
        /// @brief How many PhysicsWorlds are alive; the registration is torn down at zero.
        u32 g_LiveWorlds = 0;

        /// @brief Performs the once-per-process solver registration, refcounted by live worlds.
        ///
        /// The allocator is the solver's default: veng has no house general-purpose allocator to
        /// route it through (VMA is GPU memory and is not one).
        void AcquireSolverRegistration()
        {
            const std::scoped_lock lock(g_RegistrationMutex);
            if (g_LiveWorlds++ > 0)
            {
                return;
            }
            JPH::RegisterDefaultAllocator();
            JPH::Trace = TraceToLog;
            JPH_IF_ENABLE_ASSERTS(JPH::AssertFailed = AssertToVeAssert;)
            JPH::Factory::sInstance = new JPH::Factory();
            JPH::RegisterTypes();
        }

        /// @brief Releases one reference to the process-wide solver registration.
        void ReleaseSolverRegistration()
        {
            const std::scoped_lock lock(g_RegistrationMutex);
            VE_ASSERT(g_LiveWorlds > 0, "PhysicsWorld registration underflow");
            if (--g_LiveWorlds > 0)
            {
                return;
            }
            JPH::UnregisterTypes();
            delete JPH::Factory::sInstance;
            JPH::Factory::sInstance = nullptr;
        }

        /// @brief Maps an engine motion type onto the solver's.
        [[nodiscard]] JPH::EMotionType ToJoltMotion(const MotionType motion)
        {
            switch (motion)
            {
            case MotionType::Static:
                return JPH::EMotionType::Static;
            case MotionType::Kinematic:
                return JPH::EMotionType::Kinematic;
            case MotionType::Dynamic:
                return JPH::EMotionType::Dynamic;
            }
            VE_ASSERT(false, "Unmapped MotionType {}", static_cast<u32>(motion));
        }

        /// @brief Whether two RigidBody settings would produce the same body.
        [[nodiscard]] bool SameSettings(const RigidBody& a, const RigidBody& b)
        {
            return a.Motion == b.Motion && a.Layer == b.Layer && a.Mass == b.Mass &&
                   a.LinearDamping == b.LinearDamping && a.AngularDamping == b.AngularDamping;
        }

        /// @brief Whether two Collider settings would produce the same shape and surface.
        [[nodiscard]] bool SameSettings(const Collider& a, const Collider& b)
        {
            return a.Shape == b.Shape && a.Extents == b.Extents && a.Offset == b.Offset &&
                   a.Friction == b.Friction && a.Restitution == b.Restitution &&
                   a.Geometry.Get() == b.Geometry.Get();
        }

        /// @brief Mixes a 64-bit value, so a pose hash spreads its input bits.
        [[nodiscard]] u64 Mix(u64 value)
        {
            value ^= value >> 33;
            value *= 0xFF51AFD7ED558CCDULL;
            value ^= value >> 33;
            value *= 0xC4CEB9FE1A85EC53ULL;
            value ^= value >> 33;
            return value;
        }
    }

    namespace Detail
    {
        namespace
        {
            /// @brief Builds the solver shape a cooked CollisionShape describes.
            /// @param geometry  The cooked points and indices.
            /// @return The built shape; never null (unbuildable geometry is a fatal assert).
            [[nodiscard]] JPH::RefConst<JPH::Shape> BuildCookedShape(const CollisionShape& geometry)
            {
                JPH::Ref<JPH::ShapeSettings> settings;
                if (geometry.Geometry == CollisionGeometry::Convex)
                {
                    JPH::Array<JPH::Vec3> points;
                    points.reserve(geometry.Points.size());
                    for (const vec3 point : geometry.Points)
                    {
                        points.push_back(ToJolt(point));
                    }
                    settings = new JPH::ConvexHullShapeSettings(points);
                }
                else
                {
                    JPH::VertexList vertices;
                    vertices.reserve(geometry.Points.size());
                    for (const vec3 point : geometry.Points)
                    {
                        vertices.push_back(JPH::Float3(point.x, point.y, point.z));
                    }
                    JPH::IndexedTriangleList triangles;
                    triangles.reserve(geometry.GetTriangleCount());
                    for (usize i = 0; i + 2 < geometry.Indices.size(); i += 3)
                    {
                        triangles.push_back(JPH::IndexedTriangle(geometry.Indices[i],
                                                                 geometry.Indices[i + 1],
                                                                 geometry.Indices[i + 2], 0));
                    }
                    settings = new JPH::MeshShapeSettings(vertices, triangles);
                }

                const JPH::ShapeSettings::ShapeResult result = settings->Create();
                VE_ASSERT(result.IsValid(), "CollisionShape is not buildable: {}",
                          result.GetError().c_str());
                return result.Get();
            }
        }

        JPH::RefConst<JPH::Shape> BuildShape(const Collider& collider)
        {
            JPH::RefConst<JPH::Shape> shape;
            if (collider.Shape == ColliderShape::Mesh)
            {
                const CollisionShape* geometry = collider.Geometry.Get();
                if (geometry == nullptr)
                {
                    return {};
                }
                shape = BuildCookedShape(*geometry);
            }
            else
            {
                JPH::Ref<JPH::ShapeSettings> settings;
                switch (collider.Shape)
                {
                case ColliderShape::Box:
                {
                    // Jolt refuses a box thinner than its convex radius, so clamp the half extents
                    // up to the default radius rather than asserting inside the solver.
                    const vec3 half = glm::max(collider.Extents, vec3(JPH::cDefaultConvexRadius));
                    settings = new JPH::BoxShapeSettings(ToJolt(half));
                    break;
                }
                case ColliderShape::Sphere:
                    settings = new JPH::SphereShapeSettings(collider.Extents.x);
                    break;
                case ColliderShape::Capsule:
                    settings =
                        new JPH::CapsuleShapeSettings(collider.Extents.y, collider.Extents.x);
                    break;
                case ColliderShape::Mesh:
                    break;
                }
                VE_ASSERT(settings != nullptr, "Unmapped ColliderShape {}",
                          static_cast<u32>(collider.Shape));

                const JPH::ShapeSettings::ShapeResult result = settings->Create();
                VE_ASSERT(result.IsValid(), "Collider shape is not buildable: {}",
                          result.GetError().c_str());
                shape = result.Get();
            }

            if (collider.Offset != vec3(0.0f))
            {
                const JPH::Ref<JPH::ShapeSettings> offset = new JPH::RotatedTranslatedShapeSettings(
                    ToJolt(collider.Offset), JPH::Quat::sIdentity(), shape);
                const JPH::ShapeSettings::ShapeResult result = offset->Create();
                VE_ASSERT(result.IsValid(), "Collider offset shape is not buildable: {}",
                          result.GetError().c_str());
                shape = result.Get();
            }
            return shape;
        }
    }

    PhysicsWorld::Native::Native(const PhysicsWorldInfo& info)
        : Layers(info.Matrix), Temp(TempAllocatorBytes(info.MaxBodies))
    {
        System.Init(info.MaxBodies, AutoBodyMutexCount, info.MaxBodyPairs,
                    info.MaxContactConstraints, Layers, Layers, Layers);
        System.SetGravity(Detail::ToJolt(info.Gravity));
        Contacts.BodyOwners = &BodyOwners;
        System.SetContactListener(&Contacts);
        GravityListener.Sources = &GravitySources;
    }

    PhysicsWorld::PhysicsWorld(const PhysicsWorldInfo& info) : m_Info(info)
    {
        AcquireSolverRegistration();
        m_Native = std::make_unique<Native>(info);
    }

    PhysicsWorld::~PhysicsWorld()
    {
        DestroyAllBodies();
        m_Native.reset();
        ReleaseSolverRegistration();
    }

    Unique<PhysicsWorld> PhysicsWorld::Create(const PhysicsWorldInfo& info)
    {
        VE_ASSERT(IsSymmetric(info.Matrix),
                  "PhysicsWorldInfo::Matrix is asymmetric; a layer pair must agree in both "
                  "directions");
        VE_ASSERT(info.MaxBodies > 0, "PhysicsWorldInfo::MaxBodies must be positive");
        return Unique<PhysicsWorld>(new PhysicsWorld(info));
    }

    void PhysicsWorld::SetGravity(const vec3 gravity)
    {
        m_Info.Gravity = gravity;
        // While a field is installed it is the only source of gravity, so the solver's uniform
        // gravity stays at zero; the new constant is restored when the field is cleared.
        if (!m_Native->GravityFieldActive)
        {
            m_Native->System.SetGravity(Detail::ToJolt(gravity));
        }
    }

    vec3 PhysicsWorld::GetGravity() const
    {
        return m_Info.Gravity;
    }

    void PhysicsWorld::SetGravitySources(const std::span<const GravitySourceInstance> sources)
    {
        m_Native->GravitySources.assign(sources.begin(), sources.end());
        const bool active = !m_Native->GravitySources.empty();
        if (active == m_Native->GravityFieldActive)
        {
            return;
        }
        m_Native->GravityFieldActive = active;
        if (active)
        {
            // The field replaces the uniform gravity: zero the solver's constant so the listener's
            // per-body force is the only gravity, then a body outside every source truly free-falls.
            m_Native->System.SetGravity(JPH::Vec3::sZero());
            m_Native->System.AddStepListener(&m_Native->GravityListener);
        }
        else
        {
            m_Native->System.RemoveStepListener(&m_Native->GravityListener);
            m_Native->System.SetGravity(Detail::ToJolt(m_Info.Gravity));
        }
    }

    void PhysicsWorld::CreateBody(const Entity entity, const RigidBody& body,
                                  const Collider& collider, const PhysicsPose& pose,
                                  const bool sensor)
    {
        const bool triangleMesh = collider.Shape == ColliderShape::Mesh &&
                                  collider.Geometry.Get() != nullptr &&
                                  collider.Geometry.Get()->Geometry == CollisionGeometry::Mesh;
        VE_ASSERT(!(triangleMesh && body.Motion == MotionType::Dynamic),
                  "Entity {} carries a triangle-mesh CollisionShape on a Dynamic RigidBody; a "
                  "triangle mesh has no interior and no inertia, so Static and Kinematic are the "
                  "motion types it may back",
                  entity.Index);

        const auto existing = m_Native->Bodies.find(entity);
        if (existing != m_Native->Bodies.end())
        {
            if (SameSettings(existing->second.Body, body) &&
                SameSettings(existing->second.Shape, collider) && existing->second.Sensor == sensor)
            {
                return;
            }
            DestroyBody(entity);
        }

        const JPH::RefConst<JPH::Shape> shape = Detail::BuildShape(collider);
        if (shape == nullptr)
        {
            // A ColliderShape::Mesh collider whose geometry has not arrived yet has no shape; the
            // body is created on the tick it does.
            return;
        }

        JPH::BodyCreationSettings settings(shape, Detail::ToJolt(pose.Position),
                                           Detail::ToJolt(pose.Rotation), ToJoltMotion(body.Motion),
                                           static_cast<JPH::ObjectLayer>(body.Layer));
        settings.mFriction = collider.Friction;
        settings.mRestitution = collider.Restitution;
        settings.mLinearDamping = body.LinearDamping;
        settings.mAngularDamping = body.AngularDamping;
        // A Trigger-layer body reports overlaps and pushes nothing; that is what the layer means.
        settings.mIsSensor = sensor || body.Layer == PhysicsLayer::Trigger;
        // A sensor is told about non-dynamic bodies too, so a kinematic mover entering a trigger
        // volume is reported rather than passing through it unseen.
        settings.mCollideKinematicVsNonDynamic = settings.mIsSensor;
        if (body.Motion == MotionType::Dynamic && body.Mass > 0.0f)
        {
            settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            settings.mMassPropertiesOverride.mMass = body.Mass;
        }
        else if (triangleMesh && body.Motion == MotionType::Kinematic)
        {
            // A triangle mesh is a surface, not a solid, so the solver cannot derive mass
            // properties from it and refuses to build the motion state a kinematic body needs.
            // A kinematic body is driven rather than integrated, so the values are never read;
            // supplying a unit solid is what makes the body constructible at all.
            settings.mOverrideMassProperties = JPH::EOverrideMassProperties::MassAndInertiaProvided;
            settings.mMassPropertiesOverride.SetMassAndInertiaOfSolidBox(
                JPH::Vec3::sReplicate(1.0f), 1000.0f);
        }

        JPH::BodyInterface& bodies = m_Native->System.GetBodyInterface();
        const JPH::EActivation activation = body.Motion == MotionType::Static
                                                ? JPH::EActivation::DontActivate
                                                : JPH::EActivation::Activate;
        const JPH::BodyID id = bodies.CreateAndAddBody(settings, activation);
        VE_ASSERT(!id.IsInvalid(), "PhysicsWorld body budget exhausted (MaxBodies = {})",
                  m_Info.MaxBodies);

        m_Native->Bodies.emplace(
            entity, Detail::BodyRecord{
                        .Id = id, .Body = body, .Shape = collider, .Sensor = settings.mIsSensor});
        m_Native->BodyOwners.emplace(id.GetIndexAndSequenceNumber(), entity);
        m_Native->BroadPhaseDirty = true;
    }

    void PhysicsWorld::DestroyBody(const Entity entity)
    {
        const auto found = m_Native->Bodies.find(entity);
        if (found == m_Native->Bodies.end())
        {
            return;
        }
        // A constraint outliving one of its bodies would reference a destroyed body, so the pair
        // goes first.
        DestroyConstraint(entity);
        JPH::BodyInterface& bodies = m_Native->System.GetBodyInterface();
        bodies.RemoveBody(found->second.Id);
        bodies.DestroyBody(found->second.Id);
        m_Native->BodyOwners.erase(found->second.Id.GetIndexAndSequenceNumber());
        m_Native->Contacts.Overlaps.erase(entity);
        m_Native->Bodies.erase(found);
    }

    void PhysicsWorld::DestroyAllBodies()
    {
        for (const auto& [owner, record] : m_Native->Constraints)
        {
            m_Native->System.RemoveConstraint(record.Constraint);
        }
        m_Native->Constraints.clear();

        JPH::BodyInterface& bodies = m_Native->System.GetBodyInterface();
        for (const auto& [entity, record] : m_Native->Bodies)
        {
            bodies.RemoveBody(record.Id);
            bodies.DestroyBody(record.Id);
        }
        m_Native->Bodies.clear();
        m_Native->BodyOwners.clear();
        m_Native->Contacts.Overlaps.clear();
    }

    bool PhysicsWorld::HasBody(const Entity entity) const
    {
        return m_Native->Bodies.contains(entity);
    }

    u32 PhysicsWorld::GetBodyCount() const
    {
        return static_cast<u32>(m_Native->Bodies.size());
    }

    void PhysicsWorld::GetBodyEntities(vector<Entity>& out) const
    {
        out.clear();
        out.reserve(m_Native->Bodies.size());
        for (const auto& [entity, record] : m_Native->Bodies)
        {
            out.emplace_back(entity);
        }
        std::ranges::sort(out, [](const Entity a, const Entity b) { return a.Index < b.Index; });
    }

    void PhysicsWorld::SetBodyPose(const Entity entity, const PhysicsPose& pose)
    {
        const auto found = m_Native->Bodies.find(entity);
        if (found == m_Native->Bodies.end())
        {
            return;
        }
        const JPH::EActivation activation = found->second.Body.Motion == MotionType::Static
                                                ? JPH::EActivation::DontActivate
                                                : JPH::EActivation::Activate;
        m_Native->System.GetBodyInterface().SetPositionAndRotationWhenChanged(
            found->second.Id, Detail::ToJolt(pose.Position), Detail::ToJolt(pose.Rotation),
            activation);
    }

    void PhysicsWorld::MoveKinematicBody(const Entity entity, const PhysicsPose& target,
                                         const f32 delta)
    {
        VE_ASSERT(delta > 0.0f, "MoveKinematicBody needs a positive step length");
        const auto found = m_Native->Bodies.find(entity);
        if (found == m_Native->Bodies.end())
        {
            return;
        }
        m_Native->System.GetBodyInterface().MoveKinematic(found->second.Id,
                                                          Detail::ToJolt(target.Position),
                                                          Detail::ToJolt(target.Rotation), delta);
    }

    optional<PhysicsPose> PhysicsWorld::GetBodyPose(const Entity entity) const
    {
        const auto found = m_Native->Bodies.find(entity);
        if (found == m_Native->Bodies.end())
        {
            return std::nullopt;
        }
        const JPH::BodyInterface& bodies = m_Native->System.GetBodyInterface();
        return PhysicsPose{
            .Position = Detail::FromJolt(bodies.GetPosition(found->second.Id)),
            .Rotation = Detail::FromJolt(bodies.GetRotation(found->second.Id)),
        };
    }

    void PhysicsWorld::SetLinearVelocity(const Entity entity, const vec3 velocity)
    {
        const auto found = m_Native->Bodies.find(entity);
        if (found == m_Native->Bodies.end())
        {
            return;
        }
        m_Native->System.GetBodyInterface().SetLinearVelocity(found->second.Id,
                                                              Detail::ToJolt(velocity));
    }

    vec3 PhysicsWorld::GetLinearVelocity(const Entity entity) const
    {
        const auto found = m_Native->Bodies.find(entity);
        if (found == m_Native->Bodies.end())
        {
            return vec3(0.0f);
        }
        return Detail::FromJolt(
            m_Native->System.GetBodyInterface().GetLinearVelocity(found->second.Id));
    }

    void PhysicsWorld::SetAngularVelocity(const Entity entity, const vec3 velocity)
    {
        const auto found = m_Native->Bodies.find(entity);
        if (found == m_Native->Bodies.end())
        {
            return;
        }
        m_Native->System.GetBodyInterface().SetAngularVelocity(found->second.Id,
                                                               Detail::ToJolt(velocity));
    }

    vec3 PhysicsWorld::GetAngularVelocity(const Entity entity) const
    {
        const auto found = m_Native->Bodies.find(entity);
        if (found == m_Native->Bodies.end())
        {
            return vec3(0.0f);
        }
        return Detail::FromJolt(
            m_Native->System.GetBodyInterface().GetAngularVelocity(found->second.Id));
    }

    bool PhysicsWorld::IsBodySensor(const Entity entity) const
    {
        const auto found = m_Native->Bodies.find(entity);
        return found != m_Native->Bodies.end() && found->second.Sensor;
    }

    void PhysicsWorld::GetSensorOverlaps(const Entity sensor, const u32 layerMask,
                                         vector<Entity>& out) const
    {
        out.clear();
        const auto found = m_Native->Contacts.Overlaps.find(sensor);
        if (found == m_Native->Contacts.Overlaps.end())
        {
            return;
        }
        for (const Entity other : found->second)
        {
            const auto record = m_Native->Bodies.find(other);
            if (record == m_Native->Bodies.end())
            {
                continue;
            }
            if ((layerMask & PhysicsLayerBit(record->second.Body.Layer)) == 0)
            {
                continue;
            }
            out.emplace_back(other);
        }
        std::ranges::sort(out, [](const Entity a, const Entity b) { return a.Index < b.Index; });
        // The solver reports one manifold per touching sub-shape pair, so a body with several
        // faces against the sensor arrives more than once.
        out.erase(std::ranges::unique(out).begin(), out.end());
    }

    void PhysicsWorld::CreateConstraint(const Entity owner, const ConstraintSettings& settings)
    {
        const auto existing = m_Native->Constraints.find(owner);
        if (existing != m_Native->Constraints.end())
        {
            if (existing->second.Settings == settings)
            {
                return;
            }
            DestroyConstraint(owner);
        }

        const auto first = m_Native->Bodies.find(owner);
        const auto second = m_Native->Bodies.find(settings.Target);
        if (first == m_Native->Bodies.end() || second == m_Native->Bodies.end() ||
            owner == settings.Target)
        {
            return;
        }

        JPH::BodyInterface& bodies = m_Native->System.GetBodyInterface();
        JPH::Ref<JPH::TwoBodyConstraintSettings> constraintSettings;
        switch (settings.Kind)
        {
        case ConstraintKind::Fixed:
        {
            auto* fixed = new JPH::FixedConstraintSettings();
            // World-space anchors, so the pair is latched wherever they stand right now rather
            // than at whatever local frames the two bodies happen to carry.
            fixed->mAutoDetectPoint = true;
            constraintSettings = fixed;
            break;
        }
        case ConstraintKind::Point:
        {
            auto* point = new JPH::PointConstraintSettings();
            point->mSpace = JPH::EConstraintSpace::WorldSpace;
            point->mPoint1 = Detail::ToJolt(settings.Point);
            point->mPoint2 = Detail::ToJolt(settings.Point);
            constraintSettings = point;
            break;
        }
        case ConstraintKind::Hinge:
        {
            const vec3 axis = glm::length(settings.Axis) > 0.0f ? glm::normalize(settings.Axis)
                                                                : vec3(0.0f, 1.0f, 0.0f);
            const vec3 normal = glm::normalize(glm::cross(
                axis, std::abs(axis.y) < 0.9f ? vec3(0.0f, 1.0f, 0.0f) : vec3(1.0f, 0.0f, 0.0f)));
            auto* hinge = new JPH::HingeConstraintSettings();
            hinge->mSpace = JPH::EConstraintSpace::WorldSpace;
            hinge->mPoint1 = Detail::ToJolt(settings.Point);
            hinge->mPoint2 = Detail::ToJolt(settings.Point);
            hinge->mHingeAxis1 = Detail::ToJolt(axis);
            hinge->mHingeAxis2 = Detail::ToJolt(axis);
            hinge->mNormalAxis1 = Detail::ToJolt(normal);
            hinge->mNormalAxis2 = Detail::ToJolt(normal);
            constraintSettings = hinge;
            break;
        }
        }
        VE_ASSERT(constraintSettings != nullptr, "Unmapped ConstraintKind {}",
                  static_cast<u32>(settings.Kind));

        // The no-lock interface, because the world is only ever mutated from the thread that steps
        // it: taking two write locks that landed in one mutex bucket would deadlock.
        const JPH::BodyLockInterfaceNoLock& locks = m_Native->System.GetBodyLockInterfaceNoLock();
        const JPH::BodyLockWrite lockA(locks, first->second.Id);
        const JPH::BodyLockWrite lockB(locks, second->second.Id);
        if (!lockA.Succeeded() || !lockB.Succeeded())
        {
            return;
        }

        const JPH::Ref<JPH::TwoBodyConstraint> constraint =
            constraintSettings->Create(lockA.GetBody(), lockB.GetBody());
        m_Native->System.AddConstraint(constraint);
        m_Native->Constraints.emplace(
            owner, Detail::ConstraintRecord{.Constraint = constraint, .Settings = settings});

        // A constraint only solves on bodies the solver integrates, so both ends are woken: a
        // sleeping dynamic body would otherwise ignore the new attachment until something else
        // touched it.
        bodies.ActivateBody(first->second.Id);
        bodies.ActivateBody(second->second.Id);
    }

    void PhysicsWorld::DestroyConstraint(const Entity owner)
    {
        const auto found = m_Native->Constraints.find(owner);
        if (found == m_Native->Constraints.end())
        {
            return;
        }
        m_Native->System.RemoveConstraint(found->second.Constraint);
        m_Native->Constraints.erase(found);
    }

    bool PhysicsWorld::HasConstraint(const Entity owner) const
    {
        return m_Native->Constraints.contains(owner);
    }

    u32 PhysicsWorld::GetConstraintCount() const
    {
        return static_cast<u32>(m_Native->Constraints.size());
    }

    void PhysicsWorld::GetConstraintOwners(vector<Entity>& out) const
    {
        out.clear();
        out.reserve(m_Native->Constraints.size());
        for (const auto& [owner, record] : m_Native->Constraints)
        {
            out.emplace_back(owner);
        }
        std::ranges::sort(out, [](const Entity a, const Entity b) { return a.Index < b.Index; });
    }

    void PhysicsWorld::Step(const f32 delta)
    {
        if (m_Native->BroadPhaseDirty)
        {
            // Rebuilding the static tree is only worth it after a batch of bodies arrives, which
            // is exactly when this flag is set.
            m_Native->System.OptimizeBroadPhase();
            m_Native->BroadPhaseDirty = false;
        }

        m_Native->Contacts.DebugEnabled = m_DebugDrawEnabled;
        m_Native->Contacts.Contacts.clear();
        m_Native->Contacts.Overlaps.clear();

        const JPH::EPhysicsUpdateError error = m_Native->System.Update(
            delta, CollisionStepsPerUpdate, &m_Native->Temp, &m_Native->Jobs);
        VE_ASSERT(error == JPH::EPhysicsUpdateError::None,
                  "Physics step exceeded a world budget (error bits {})", static_cast<u32>(error));
        ++m_StepCount;
    }

    vector<u8> PhysicsWorld::SaveState() const
    {
        JPH::StateRecorderImpl recorder;
        m_Native->System.SaveState(recorder);
        const std::string data = recorder.GetData();
        const auto* bytes = reinterpret_cast<const u8*>(data.data());
        return {bytes, bytes + data.size()};
    }

    VoidResult PhysicsWorld::RestoreState(const std::span<const u8> state)
    {
        JPH::StateRecorderImpl recorder;
        recorder.WriteBytes(state.data(), state.size());
        recorder.Rewind();
        // The solver reports only its own consistency checks; a stream that ran out mid-read is a
        // failure it does not see, so the recorder's own status is consulted too.
        if (!m_Native->System.RestoreState(recorder) || recorder.IsFailed())
        {
            return std::unexpected(
                "PhysicsWorld::RestoreState: the bytes are not a readable state");
        }
        return {};
    }

    u64 PhysicsWorld::HashPoses() const
    {
        const JPH::BodyInterface& bodies = m_Native->System.GetBodyInterface();
        u64 hash = 0;
        for (const auto& [entity, record] : m_Native->Bodies)
        {
            const JPH::RVec3 position = bodies.GetPosition(record.Id);
            const JPH::Quat rotation = bodies.GetRotation(record.Id);

            // Hash the raw bit patterns, so two worlds hash equal only when their bodies are
            // bit-identically placed. The entity index rides along so two bodies at the same pose
            // do not cancel in the order-independent fold below.
            u64 body = Mix(entity.Index);
            for (const f64 component :
                 {static_cast<f64>(position.GetX()), static_cast<f64>(position.GetY()),
                  static_cast<f64>(position.GetZ())})
            {
                u64 bits = 0;
                std::memcpy(&bits, &component, sizeof(bits));
                body = Mix(body ^ bits);
            }
            for (const f32 component :
                 {rotation.GetX(), rotation.GetY(), rotation.GetZ(), rotation.GetW()})
            {
                u32 bits = 0;
                std::memcpy(&bits, &component, sizeof(bits));
                body = Mix(body ^ bits);
            }

            // Addition rather than XOR: the fold must not depend on the map's iteration order, and
            // must not cancel when two bodies happen to hash alike.
            hash += body;
        }
        return hash;
    }

    void PhysicsWorld::DrawDebug(Renderer::DebugDraw& sink) const
    {
        const vec4 StaticColor(0.35f, 0.55f, 0.35f, 1.0f);
        const vec4 KinematicColor(0.35f, 0.55f, 0.90f, 1.0f);
        const vec4 AwakeColor(0.95f, 0.75f, 0.25f, 1.0f);
        const vec4 AsleepColor(0.45f, 0.40f, 0.30f, 1.0f);
        const vec4 ContactColor(1.0f, 0.25f, 0.25f, 1.0f);
        constexpr f32 ContactSpikeLength = 0.25f;

        const JPH::BodyInterface& bodies = m_Native->System.GetBodyInterface();
        for (const auto& [entity, record] : m_Native->Bodies)
        {
            const vec3 center = vec3(Detail::FromJolt(bodies.GetPosition(record.Id)));
            const quat rotation = Detail::FromJolt(bodies.GetRotation(record.Id));

            vec4 color = StaticColor;
            if (record.Body.Motion == MotionType::Kinematic)
            {
                color = KinematicColor;
            }
            else if (record.Body.Motion == MotionType::Dynamic)
            {
                color = bodies.IsActive(record.Id) ? AwakeColor : AsleepColor;
            }

            const vec3 origin = center + rotation * record.Shape.Offset;
            switch (record.Shape.Shape)
            {
            case ColliderShape::Box:
            {
                const vec3 half = record.Shape.Extents;
                vec3 corners[8];
                for (u32 i = 0; i < 8; ++i)
                {
                    const vec3 sign((i & 1U) != 0 ? 1.0f : -1.0f, (i & 2U) != 0 ? 1.0f : -1.0f,
                                    (i & 4U) != 0 ? 1.0f : -1.0f);
                    corners[i] = origin + rotation * (sign * half);
                }
                // Two corner indices differ in exactly one bit iff they share an edge.
                for (u32 a = 0; a < 8; ++a)
                {
                    for (u32 bit = 1; bit < 8; bit <<= 1)
                    {
                        const u32 b = a | bit;
                        if (b != a)
                        {
                            sink.DrawLine(corners[a], corners[b], color);
                        }
                    }
                }
                break;
            }
            case ColliderShape::Sphere:
                sink.DrawSphere(origin, record.Shape.Extents.x, color);
                break;
            case ColliderShape::Capsule:
            {
                const f32 radius = record.Shape.Extents.x;
                const vec3 axis = rotation * vec3(0.0f, record.Shape.Extents.y, 0.0f);
                sink.DrawSphere(origin + axis, radius, color);
                sink.DrawSphere(origin - axis, radius, color);
                for (const vec3 side : {vec3(1.0f, 0.0f, 0.0f), vec3(-1.0f, 0.0f, 0.0f),
                                        vec3(0.0f, 0.0f, 1.0f), vec3(0.0f, 0.0f, -1.0f)})
                {
                    const vec3 offset = rotation * (side * radius);
                    sink.DrawLine(origin + axis + offset, origin - axis + offset, color);
                }
                break;
            }
            case ColliderShape::Mesh:
            {
                // Cooked geometry is drawn as its triangle edges, or as its hull points' bounding
                // box when the shape is a convex point cloud with no edge list of its own.
                const CollisionShape* geometry = record.Shape.Geometry.Get();
                if (geometry == nullptr)
                {
                    break;
                }
                if (geometry->Geometry == CollisionGeometry::Mesh)
                {
                    for (usize i = 0; i + 2 < geometry->Indices.size(); i += 3)
                    {
                        const vec3 a = origin + rotation * geometry->Points[geometry->Indices[i]];
                        const vec3 b =
                            origin + rotation * geometry->Points[geometry->Indices[i + 1]];
                        const vec3 c =
                            origin + rotation * geometry->Points[geometry->Indices[i + 2]];
                        sink.DrawLine(a, b, color);
                        sink.DrawLine(b, c, color);
                        sink.DrawLine(c, a, color);
                    }
                }
                else
                {
                    for (const vec3 point : geometry->Points)
                    {
                        const vec3 world = origin + rotation * point;
                        sink.DrawLine(world - vec3(0.05f, 0.0f, 0.0f),
                                      world + vec3(0.05f, 0.0f, 0.0f), color);
                        sink.DrawLine(world - vec3(0.0f, 0.05f, 0.0f),
                                      world + vec3(0.0f, 0.05f, 0.0f), color);
                        sink.DrawLine(world - vec3(0.0f, 0.0f, 0.05f),
                                      world + vec3(0.0f, 0.0f, 0.05f), color);
                    }
                }
                break;
            }
            }
        }

        for (const Detail::DebugContact& contact : m_Native->Contacts.Contacts)
        {
            sink.DrawLine(contact.Point, contact.Point + contact.Normal * ContactSpikeLength,
                          ContactColor);
        }
    }
}
