#include <Veng/Physics/PhysicsWorld.h>

#include <Veng/Assert.h>
#include <Veng/Log.h>
#include <Veng/Renderer/DebugDraw.h>

#include <Jolt/Jolt.h>

#include <Jolt/Core/Factory.h>
#include <Jolt/Core/IssueReporting.h>
#include <Jolt/Core/JobSystemSingleThreaded.h>
#include <Jolt/Core/TempAllocator.h>
#include <Jolt/Physics/Body/BodyCreationSettings.h>
#include <Jolt/Physics/Body/BodyInterface.h>
#include <Jolt/Physics/Collision/BroadPhase/BroadPhaseLayer.h>
#include <Jolt/Physics/Collision/ContactListener.h>
#include <Jolt/Physics/Collision/ObjectLayer.h>
#include <Jolt/Physics/Collision/Shape/BoxShape.h>
#include <Jolt/Physics/Collision/Shape/CapsuleShape.h>
#include <Jolt/Physics/Collision/Shape/RotatedTranslatedShape.h>
#include <Jolt/Physics/Collision/Shape/SphereShape.h>
#include <Jolt/Physics/PhysicsSettings.h>
#include <Jolt/Physics/PhysicsSystem.h>
#include <Jolt/Physics/StateRecorderImpl.h>
#include <Jolt/RegisterTypes.h>

#include <algorithm>
#include <cstdarg>
#include <cstdio>
#include <cstring>
#include <mutex>
#include <unordered_map>

namespace Veng
{
    namespace
    {
        /// @brief Broad-phase layer holding bodies that never move.
        constexpr JPH::BroadPhaseLayer BroadPhaseNonMoving(0);
        /// @brief Broad-phase layer holding bodies that can move.
        constexpr JPH::BroadPhaseLayer BroadPhaseMoving(1);
        /// @brief Number of broad-phase layers the world declares.
        constexpr u32 BroadPhaseLayerCount = 2;

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

        /// @brief Converts an engine world-space position into the solver's real-precision vector.
        [[nodiscard]] JPH::RVec3 ToJolt(const dvec3 value)
        {
            return {static_cast<JPH::Real>(value.x), static_cast<JPH::Real>(value.y),
                    static_cast<JPH::Real>(value.z)};
        }

        /// @brief Converts an engine direction or velocity into the solver's single-precision vector.
        [[nodiscard]] JPH::Vec3 ToJolt(const vec3 value)
        {
            return {value.x, value.y, value.z};
        }

        /// @brief Converts an engine orientation into the solver's quaternion (x, y, z, w order).
        [[nodiscard]] JPH::Quat ToJolt(const quat value)
        {
            return {value.x, value.y, value.z, value.w};
        }

        /// @brief Converts a solver real-precision position into an engine world-space position.
        [[nodiscard]] dvec3 FromJolt(const JPH::RVec3 value)
        {
            return {static_cast<f64>(value.GetX()), static_cast<f64>(value.GetY()),
                    static_cast<f64>(value.GetZ())};
        }

        /// @brief Converts a solver single-precision vector into an engine vector.
        [[nodiscard]] vec3 FromJolt(const JPH::Vec3 value)
        {
            return {value.GetX(), value.GetY(), value.GetZ()};
        }

        /// @brief Converts a solver quaternion into an engine orientation.
        [[nodiscard]] quat FromJolt(const JPH::Quat value)
        {
            return {value.GetW(), value.GetX(), value.GetY(), value.GetZ()};
        }

        /// @brief Maps an engine motion type onto the solver's.
        [[nodiscard]] JPH::EMotionType ToJolt(const MotionType motion)
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
                   a.Friction == b.Friction && a.Restitution == b.Restitution;
        }

        /// @brief Builds the solver shape a Collider describes, offset into the body's frame.
        /// @param collider  The primitive to build.
        /// @return The built shape; never null (an invalid primitive is a fatal assert).
        [[nodiscard]] JPH::RefConst<JPH::Shape> BuildShape(const Collider& collider)
        {
            JPH::Ref<JPH::ShapeSettings> settings;
            switch (collider.Shape)
            {
            case ColliderShape::Box:
            {
                // Jolt refuses a box thinner than its convex radius, so clamp the half extents up
                // to the default radius rather than asserting inside the solver.
                const vec3 half = glm::max(collider.Extents, vec3(JPH::cDefaultConvexRadius));
                settings = new JPH::BoxShapeSettings(ToJolt(half));
                break;
            }
            case ColliderShape::Sphere:
                settings = new JPH::SphereShapeSettings(collider.Extents.x);
                break;
            case ColliderShape::Capsule:
                settings = new JPH::CapsuleShapeSettings(collider.Extents.y, collider.Extents.x);
                break;
            }
            VE_ASSERT(settings != nullptr, "Unmapped ColliderShape {}",
                      static_cast<u32>(collider.Shape));

            if (collider.Offset != vec3(0.0f))
            {
                settings = new JPH::RotatedTranslatedShapeSettings(
                    ToJolt(collider.Offset), JPH::Quat::sIdentity(), settings);
            }

            const JPH::ShapeSettings::ShapeResult result = settings->Create();
            VE_ASSERT(result.IsValid(), "Collider shape is not buildable: {}",
                      result.GetError().c_str());
            return result.Get();
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

        /// @brief Collects contact points during a step, for the debug visualization.
        ///
        /// Recording is gated by Enabled so a world with the visualization off pays nothing beyond
        /// the virtual call. It observes contacts and never changes settings, so it cannot perturb
        /// the simulation.
        class ContactRecorder final : public JPH::ContactListener
        {
        public:
            /// @brief Whether contacts are recorded this step.
            bool Enabled = false;
            /// @brief Contacts recorded during the current step.
            vector<DebugContact> Contacts;

            /// @brief Records a newly detected contact manifold.
            /// @param manifold  The manifold's points and normal.
            void OnContactAdded(const JPH::Body&, const JPH::Body&,
                                const JPH::ContactManifold& manifold,
                                JPH::ContactSettings&) override
            {
                Record(manifold);
            }

            /// @brief Records a contact manifold that persists from the previous step.
            /// @param manifold  The manifold's points and normal.
            void OnContactPersisted(const JPH::Body&, const JPH::Body&,
                                    const JPH::ContactManifold& manifold,
                                    JPH::ContactSettings&) override
            {
                Record(manifold);
            }

        private:
            /// @brief Appends every point of @p manifold to the recorded set.
            /// @param manifold  The manifold to record.
            void Record(const JPH::ContactManifold& manifold)
            {
                if (!Enabled)
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
        };
    }

    struct PhysicsWorld::Native
    {
        /// @brief Builds the backend state from the world's descriptor.
        /// @param info  Gravity, the collision matrix, and the body budget.
        explicit Native(const PhysicsWorldInfo& info)
            : Layers(info.Matrix), Temp(TempAllocatorBytes(info.MaxBodies))
        {
            System.Init(info.MaxBodies, AutoBodyMutexCount, info.MaxBodyPairs,
                        info.MaxContactConstraints, Layers, Layers, Layers);
            System.SetGravity(ToJolt(info.Gravity));
            System.SetContactListener(&Contacts);
        }

        /// @brief The broad-phase mapping and both collision filters.
        LayerTable Layers;
        /// @brief Scratch the solver carves its per-step allocations out of.
        JPH::TempAllocatorImpl Temp;
        /// @brief The solver's job system, run on the calling thread.
        ///
        /// Single-threaded: veng's render thread is single and sharing a pool with the TaskSystem
        /// is a separate design question, so the step runs entirely on its caller.
        JPH::JobSystemSingleThreaded Jobs{JPH::cMaxPhysicsJobs};
        /// @brief The solver itself.
        JPH::PhysicsSystem System;
        /// @brief The contact collector feeding the debug visualization.
        ContactRecorder Contacts;
        /// @brief Live bodies, keyed by the entity that owns them.
        std::unordered_map<Entity, BodyRecord> Bodies;
        /// @brief Whether a body was created since the last step, so the broad phase is re-optimized.
        bool BroadPhaseDirty = false;
    };

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
        m_Native->System.SetGravity(ToJolt(gravity));
    }

    vec3 PhysicsWorld::GetGravity() const
    {
        return FromJolt(m_Native->System.GetGravity());
    }

    void PhysicsWorld::CreateBody(const Entity entity, const RigidBody& body,
                                  const Collider& collider, const PhysicsPose& pose)
    {
        const auto existing = m_Native->Bodies.find(entity);
        if (existing != m_Native->Bodies.end())
        {
            if (SameSettings(existing->second.Body, body) &&
                SameSettings(existing->second.Shape, collider))
            {
                return;
            }
            DestroyBody(entity);
        }

        JPH::BodyCreationSettings settings(BuildShape(collider), ToJolt(pose.Position),
                                           ToJolt(pose.Rotation), ToJolt(body.Motion),
                                           static_cast<JPH::ObjectLayer>(body.Layer));
        settings.mFriction = collider.Friction;
        settings.mRestitution = collider.Restitution;
        settings.mLinearDamping = body.LinearDamping;
        settings.mAngularDamping = body.AngularDamping;
        // A Trigger-layer body reports overlaps and pushes nothing; that is what the layer means.
        settings.mIsSensor = body.Layer == PhysicsLayer::Trigger;
        if (body.Motion == MotionType::Dynamic && body.Mass > 0.0f)
        {
            settings.mOverrideMassProperties = JPH::EOverrideMassProperties::CalculateInertia;
            settings.mMassPropertiesOverride.mMass = body.Mass;
        }

        JPH::BodyInterface& bodies = m_Native->System.GetBodyInterface();
        const JPH::EActivation activation = body.Motion == MotionType::Static
                                                ? JPH::EActivation::DontActivate
                                                : JPH::EActivation::Activate;
        const JPH::BodyID id = bodies.CreateAndAddBody(settings, activation);
        VE_ASSERT(!id.IsInvalid(), "PhysicsWorld body budget exhausted (MaxBodies = {})",
                  m_Info.MaxBodies);

        m_Native->Bodies.emplace(entity, BodyRecord{.Id = id, .Body = body, .Shape = collider});
        m_Native->BroadPhaseDirty = true;
    }

    void PhysicsWorld::DestroyBody(const Entity entity)
    {
        const auto found = m_Native->Bodies.find(entity);
        if (found == m_Native->Bodies.end())
        {
            return;
        }
        JPH::BodyInterface& bodies = m_Native->System.GetBodyInterface();
        bodies.RemoveBody(found->second.Id);
        bodies.DestroyBody(found->second.Id);
        m_Native->Bodies.erase(found);
    }

    void PhysicsWorld::DestroyAllBodies()
    {
        JPH::BodyInterface& bodies = m_Native->System.GetBodyInterface();
        for (const auto& [entity, record] : m_Native->Bodies)
        {
            bodies.RemoveBody(record.Id);
            bodies.DestroyBody(record.Id);
        }
        m_Native->Bodies.clear();
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
            found->second.Id, ToJolt(pose.Position), ToJolt(pose.Rotation), activation);
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
        m_Native->System.GetBodyInterface().MoveKinematic(found->second.Id, ToJolt(target.Position),
                                                          ToJolt(target.Rotation), delta);
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
            .Position = FromJolt(bodies.GetPosition(found->second.Id)),
            .Rotation = FromJolt(bodies.GetRotation(found->second.Id)),
        };
    }

    void PhysicsWorld::SetLinearVelocity(const Entity entity, const vec3 velocity)
    {
        const auto found = m_Native->Bodies.find(entity);
        if (found == m_Native->Bodies.end())
        {
            return;
        }
        m_Native->System.GetBodyInterface().SetLinearVelocity(found->second.Id, ToJolt(velocity));
    }

    vec3 PhysicsWorld::GetLinearVelocity(const Entity entity) const
    {
        const auto found = m_Native->Bodies.find(entity);
        if (found == m_Native->Bodies.end())
        {
            return vec3(0.0f);
        }
        return FromJolt(m_Native->System.GetBodyInterface().GetLinearVelocity(found->second.Id));
    }

    void PhysicsWorld::SetAngularVelocity(const Entity entity, const vec3 velocity)
    {
        const auto found = m_Native->Bodies.find(entity);
        if (found == m_Native->Bodies.end())
        {
            return;
        }
        m_Native->System.GetBodyInterface().SetAngularVelocity(found->second.Id, ToJolt(velocity));
    }

    vec3 PhysicsWorld::GetAngularVelocity(const Entity entity) const
    {
        const auto found = m_Native->Bodies.find(entity);
        if (found == m_Native->Bodies.end())
        {
            return vec3(0.0f);
        }
        return FromJolt(m_Native->System.GetBodyInterface().GetAngularVelocity(found->second.Id));
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

        m_Native->Contacts.Enabled = m_DebugDrawEnabled;
        m_Native->Contacts.Contacts.clear();

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
            const vec3 center = vec3(FromJolt(bodies.GetPosition(record.Id)));
            const quat rotation = FromJolt(bodies.GetRotation(record.Id));

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
            }
        }

        for (const DebugContact& contact : m_Native->Contacts.Contacts)
        {
            sink.DrawLine(contact.Point, contact.Point + contact.Normal * ContactSpikeLength,
                          ContactColor);
        }
    }
}
