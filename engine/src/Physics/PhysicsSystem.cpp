#include <Veng/Physics/PhysicsSystem.h>

#include <Veng/Diagnostics/Profiler.h>
#include <Veng/Physics/Components.h>
#include <Veng/Physics/PhysicsWorld.h>
#include <Veng/Renderer/DebugDraw.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include "PhysicsInternal.h"

#include <algorithm>
#include <iterator>

namespace Veng
{
    namespace
    {
        /// @brief Reads an entity's authoritative pose for this tick.
        ///
        /// With SyncTransform set the Transform is the pose (world space — a physics entity is a
        /// scene-graph root); cleared, the entity's PhysicsPose is the only channel.
        /// @param scene          The scene the entity lives in.
        /// @param entity         The entity to read.
        /// @param syncTransform  The entity's RigidBody::SyncTransform.
        /// @return The pose the solver should adopt.
        [[nodiscard]] PhysicsPose ReadPose(const Scene& scene, const Entity entity,
                                           const bool syncTransform)
        {
            if (syncTransform)
            {
                if (const auto* transform = scene.TryGet<Transform>(entity))
                {
                    return PhysicsPose{
                        .Position = dvec3(transform->Position),
                        .Rotation = transform->Rotation,
                    };
                }
            }
            if (const auto* pose = scene.TryGet<PhysicsPose>(entity))
            {
                return *pose;
            }
            return {};
        }

        /// @brief Whether a collider's shape is buildable this tick.
        ///
        /// A ColliderShape::Mesh collider whose cooked geometry has not arrived has no shape at
        /// all, so its entity is skipped exactly as a RigidBody with no Collider is.
        /// @param collider  The collider to test.
        [[nodiscard]] bool HasShape(const Collider& collider)
        {
            return collider.Shape != ColliderShape::Mesh || collider.Geometry.Get() != nullptr;
        }

        /// @brief Reconciles the scene's constraint components against the world's constraints.
        ///
        /// Runs after the body pass, so a constraint stamped in the same tick as its bodies finds
        /// them; a constraint naming an entity that has no body yet is left for a later tick.
        /// @param scene  The scene whose constraint components are read.
        /// @param world  The world whose constraints are brought into line.
        void ReconcileConstraints(const Scene& scene, PhysicsWorld& world)
        {
            vector<Entity> owners;
            for (auto [entity, constraint] : scene.View<FixedConstraint>())
            {
                world.CreateConstraint(entity, ConstraintSettings{
                                                   .Kind = ConstraintKind::Fixed,
                                                   .Target = constraint.Target,
                                               });
                owners.emplace_back(entity);
            }
            for (auto [entity, constraint] : scene.View<PointConstraint>())
            {
                world.CreateConstraint(entity, ConstraintSettings{
                                                   .Kind = ConstraintKind::Point,
                                                   .Target = constraint.Target,
                                                   .Point = dvec3(constraint.Point),
                                               });
                owners.emplace_back(entity);
            }
            for (auto [entity, constraint] : scene.View<HingeConstraint>())
            {
                world.CreateConstraint(entity, ConstraintSettings{
                                                   .Kind = ConstraintKind::Hinge,
                                                   .Target = constraint.Target,
                                                   .Point = dvec3(constraint.Point),
                                                   .Axis = constraint.Axis,
                                               });
                owners.emplace_back(entity);
            }

            vector<Entity> resident;
            world.GetConstraintOwners(resident);
            for (const Entity entity : resident)
            {
                if (std::ranges::find(owners, entity) == owners.end())
                {
                    world.DestroyConstraint(entity);
                }
            }
        }

        /// @brief Whether the scene carries any Predicted body — a body that rolls back.
        ///
        /// Gates whether the step runs during a reconciliation replay: a scene with none does not
        /// participate in rollback and is skipped, a scene predicting a character re-drives its
        /// kinematic world so the character re-collides against the replayed tick's configuration.
        /// @param scene  The scene to test.
        [[nodiscard]] bool HasPredictedBody(const Scene& scene)
        {
            for (auto [entity, predicted] : scene.View<Predicted>())
            {
                (void)entity;
                (void)predicted;
                return true;
            }
            return false;
        }

        /// @brief Whether two poses are bit-for-bit the same placement.
        [[nodiscard]] bool SamePose(const PhysicsPose& a, const PhysicsPose& b)
        {
            return a.Position == b.Position && a.Rotation == b.Rotation;
        }

        /// @brief Brings one entity's body into line with its components.
        ///
        /// Creates or re-creates the body when the entity carries a RigidBody and a Collider with a
        /// shape, destroys it otherwise, and parks an entity whose cooked geometry has not arrived
        /// until it does. A body made here seeds the entity's PhysicsPose, since a body created
        /// asleep, or static, is never read back until it moves.
        /// @param scene   The scene the entity lives in.
        /// @param world   The world whose body is brought into line.
        /// @param entity  The entity to reconcile.
        void ReconcileEntity(Scene& scene, PhysicsWorld& world, const Entity entity)
        {
            PhysicsWorld::Native& native = world.GetNative();
            const Scene& readScene = scene;
            const RigidBody* body =
                scene.IsAlive(entity) ? readScene.TryGet<RigidBody>(entity) : nullptr;
            const Collider* collider =
                body != nullptr ? readScene.TryGet<Collider>(entity) : nullptr;
            if (body == nullptr || collider == nullptr)
            {
                world.DestroyBody(entity);
                return;
            }
            if (!HasShape(*collider))
            {
                world.DestroyBody(entity);
                if (std::ranges::find(native.AwaitingShape, entity) == native.AwaitingShape.end())
                {
                    native.AwaitingShape.emplace_back(entity);
                }
                return;
            }

            // Copied: the components added below may move what these point at.
            const RigidBody settings = *body;
            const Collider shape = *collider;
            const bool sensor = readScene.Has<Sensor>(entity);
            const PhysicsPose pose = ReadPose(readScene, entity, settings.SyncTransform);

            const auto before = native.Bodies.find(entity);
            const JPH::BodyID previous =
                before != native.Bodies.end() ? before->second.Id : JPH::BodyID();
            world.CreateBody(entity, settings, shape, pose, sensor);
            const auto after = native.Bodies.find(entity);
            const bool built = after != native.Bodies.end() && after->second.Id != previous;

            if (!readScene.Has<PhysicsPose>(entity))
            {
                scene.Add<PhysicsPose>(entity, pose);
            }
            else if (built && settings.SyncTransform)
            {
                scene.Get<PhysicsPose>(entity) = pose;
            }
            if (sensor && !readScene.Has<SensorOverlaps>(entity))
            {
                scene.Add<SensorOverlaps>(entity);
            }
        }

        /// @brief Reconciles each entity in @p work once, then empties it.
        /// @param scene  The scene whose components are the authority.
        /// @param world  The world whose bodies are brought into line.
        /// @param work   The entities to reconcile, in any order and possibly repeated.
        /// @param stats  The step's counters.
        void ReconcileEach(Scene& scene, PhysicsWorld& world, vector<Entity>& work,
                           PhysicsStepStats& stats)
        {
            // A removal hook and a change stamp can both name one entity, so it is visited once.
            std::ranges::sort(
                work, [](const Entity a, const Entity b)
                { return a.Index != b.Index ? a.Index < b.Index : a.Generation < b.Generation; });
            work.erase(std::ranges::unique(work).begin(), work.end());
            for (const Entity entity : work)
            {
                ReconcileEntity(scene, world, entity);
            }
            stats.Reconciled += static_cast<u32>(work.size());
            work.clear();
        }

        /// @brief Destroys every body whose entity is gone or has lost its components.
        /// @param scene  The scene whose components are the authority.
        /// @param world  The world whose orphans are destroyed.
        /// @param stats  The step's counters.
        void SweepOrphans(const Scene& scene, PhysicsWorld& world, PhysicsStepStats& stats)
        {
            vector<Entity> resident;
            world.GetBodyEntities(resident);
            for (const Entity entity : resident)
            {
                const Collider* collider =
                    scene.IsAlive(entity) ? scene.TryGet<Collider>(entity) : nullptr;
                if (collider == nullptr || !scene.Has<RigidBody>(entity) || !HasShape(*collider))
                {
                    world.DestroyBody(entity);
                    ++stats.Reconciled;
                }
            }
        }

        /// @brief The reconcile pass: brings every body whose components changed into line.
        ///
        /// A component stamped at or after the tick the last reconcile ran at is a change, found by
        /// one pass over each physics pool's packed change ticks, so the pass touches only the
        /// entities whose RigidBody, Collider or Sensor moved. Removals reach it through the
        /// components' removal hook (Pending), and an entity waiting on cooked geometry is re-checked
        /// each step. The first step, a step after a replay or a DestroyAllBodies, and any step
        /// whose scene tick has gone backwards walk every physics entity instead and sweep orphans,
        /// since change ticks cannot say what moved then.
        /// @param scene  The scene whose components are the authority.
        /// @param world  The world whose bodies are brought into line.
        /// @param stats  The step's counters.
        void Reconcile(Scene& scene, PhysicsWorld& world, PhysicsStepStats& stats)
        {
            PhysicsWorld::Native& native = world.GetNative();
            const Scene& readScene = scene;
            const u64 now = scene.GetChangeTick();
            const u64 since = native.ReconciledTick;
            const bool full = native.FullReconcile || now < since;

            vector<Entity> work = std::move(native.Pending);
            native.Pending.clear();

            // An entity waiting on its cooked geometry is re-checked once the geometry arrives or
            // the entity stops asking for it.
            std::erase_if(native.AwaitingShape,
                          [&](const Entity entity)
                          {
                              const Collider* collider = scene.IsAlive(entity)
                                                             ? readScene.TryGet<Collider>(entity)
                                                             : nullptr;
                              if (collider != nullptr && !HasShape(*collider))
                              {
                                  return false;
                              }
                              work.emplace_back(entity);
                              return true;
                          });

            // Collected before any is reconciled: reconciling adds PhysicsPose, a structural change
            // the walks below must not see mid-iteration.
            const auto collect = [&work](const Entity entity) { work.emplace_back(entity); };
            if (full)
            {
                for (auto [entity, body, collider] : readScene.View<RigidBody, Collider>())
                {
                    (void)body;
                    (void)collider;
                    work.emplace_back(entity);
                }
            }
            else
            {
                readScene.ForEachChangedSince<RigidBody>(since, collect);
                readScene.ForEachChangedSince<Collider>(since, collect);
                readScene.ForEachChangedSince<Sensor>(since, collect);
            }
            ReconcileEach(scene, world, work, stats);
            if (full)
            {
                SweepOrphans(readScene, world, stats);
            }

            stats.FullReconcile = full;
            native.FullReconcile = false;
            native.ReconciledTick = now;
        }

        /// @brief The push pass: places each static or kinematic body whose pose changed.
        ///
        /// A body is pushed when the pose its source component holds differs from the one it was
        /// last placed at. A kinematic body is swept toward its target, which sets the velocity
        /// that reaches it; the solver keeps integrating that velocity, so on the first tick its
        /// pose holds still the body is pushed once more to the same pose, zeroing the velocity,
        /// and after that it is left to sleep. A dynamic body's pose is the solver's.
        /// @param scene  The scene the poses are read from.
        /// @param world  The world whose bodies are placed.
        /// @param delta  The step length in seconds.
        /// @param stats  The step's counters.
        void Push(Scene& scene, const PhysicsWorld& world, const f32 delta, PhysicsStepStats& stats)
        {
            PhysicsWorld::Native& native = world.GetNative();
            const Scene& readScene = scene;
            JPH::BodyInterface& bodies = native.System.GetBodyInterface();
            for (Detail::DrivenBody& driven : native.Driven)
            {
                const PhysicsPose target = ReadPose(readScene, driven.Owner, driven.SyncTransform);
                if (!SamePose(target, driven.LastPushed))
                {
                    driven.LastPushed = target;
                    if (driven.Motion == MotionType::Kinematic)
                    {
                        bodies.MoveKinematic(driven.Id, Detail::ToJolt(target.Position),
                                             Detail::ToJolt(target.Rotation), delta);
                        driven.Moving = true;
                    }
                    else
                    {
                        bodies.SetPositionAndRotation(driven.Id, Detail::ToJolt(target.Position),
                                                      Detail::ToJolt(target.Rotation),
                                                      JPH::EActivation::DontActivate);
                        // A static body is never awake, so it is never read back: its
                        // PhysicsPose follows here.
                        if (driven.SyncTransform)
                        {
                            if (auto* pose = scene.TryGet<PhysicsPose>(driven.Owner))
                            {
                                *pose = target;
                            }
                        }
                    }
                    ++stats.Pushed;
                }
                else if (driven.Moving)
                {
                    bodies.MoveKinematic(driven.Id, Detail::ToJolt(target.Position),
                                         Detail::ToJolt(target.Rotation), delta);
                    driven.Moving = false;
                    ++stats.Pushed;
                }
            }
        }

        /// @brief Reads one body's pose back into its entity's components.
        /// @param scene   The scene the entity lives in.
        /// @param native  The world's backend state.
        /// @param id      The body to read.
        /// @return Whether a pose was written.
        bool ReadBackBody(Scene& scene, PhysicsWorld::Native& native, const JPH::BodyID& id)
        {
            const JPH::Body* body = native.System.GetBodyLockInterfaceNoLock().TryGetBody(id);
            if (body == nullptr)
            {
                return false;
            }
            const Entity entity = Detail::UnpackEntity(body->GetUserData());
            const auto record = native.Bodies.find(entity);
            if (record == native.Bodies.end() || record->second.Id != id || !scene.IsAlive(entity))
            {
                return false;
            }

            const PhysicsPose pose{
                .Position = Detail::FromJolt(body->GetPosition()),
                .Rotation = Detail::FromJolt(body->GetRotation()),
            };
            if (auto* stored = scene.TryGet<PhysicsPose>(entity))
            {
                *stored = pose;
            }
            else
            {
                scene.Add<PhysicsPose>(entity, pose);
            }

            const RigidBody& settings = record->second.Body;
            if (settings.SyncTransform && settings.Motion != MotionType::Static)
            {
                if (auto* transform = scene.TryGet<Transform>(entity))
                {
                    transform->Position = vec3(pose.Position);
                    transform->Rotation = pose.Rotation;
                }
            }
            // What was just written is where the body is, so a kinematic body whose owner leaves it
            // there is not pushed again.
            if (const u32 slot = record->second.Driven; slot != Detail::NotDriven)
            {
                Detail::DrivenBody& driven = native.Driven[slot];
                driven.LastPushed = ReadPose(scene, entity, driven.SyncTransform);
            }
            return true;
        }

        /// @brief The pull pass: reads back the bodies the step moved.
        ///
        /// Those are the bodies still awake after the step, plus every body that went to sleep
        /// during it — a body that settles drops out of the active list, so without those its
        /// final pose would never be read. A sleeping body's PhysicsPose is left as it is.
        /// @param scene  The scene the poses are written to.
        /// @param world  The world that stepped.
        /// @param stats  The step's counters.
        void ReadBack(Scene& scene, const PhysicsWorld& world, PhysicsStepStats& stats)
        {
            PhysicsWorld::Native& native = world.GetNative();
            if (native.FullReadBack)
            {
                for (const auto& [entity, record] : native.Bodies)
                {
                    stats.ReadBack += ReadBackBody(scene, native, record.Id) ? 1 : 0;
                }
                native.FullReadBack = false;
                native.ReadBackNext.clear();
                return;
            }

            // The solver's job system runs on this thread and the step has returned, so the active
            // list is stable while it is walked.
            const JPH::uint active = native.System.GetNumActiveBodies(JPH::EBodyType::RigidBody);
            const JPH::BodyID* ids = native.System.GetActiveBodiesUnsafe(JPH::EBodyType::RigidBody);
            for (JPH::uint i = 0; i < active; ++i)
            {
                stats.ReadBack += ReadBackBody(scene, native, ids[i]) ? 1 : 0;
            }
            for (const JPH::BodyID& id : native.Activations.Deactivated)
            {
                stats.ReadBack += ReadBackBody(scene, native, id) ? 1 : 0;
            }
            for (const Entity entity : native.ReadBackNext)
            {
                const auto record = native.Bodies.find(entity);
                if (record != native.Bodies.end() &&
                    !native.System.GetBodyInterface().IsActive(record->second.Id))
                {
                    stats.ReadBack += ReadBackBody(scene, native, record->second.Id) ? 1 : 0;
                }
            }
            native.ReadBackNext.clear();
        }

        /// @brief Publishes each sensor's overlap set and this tick's enter/exit deltas.
        ///
        /// State a system drains, not an event it subscribes to: the deltas are computed here,
        /// after the step, so no gameplay code runs inside the solver.
        /// @param scene  The scene whose Sensor entities are published to.
        /// @param world  The world holding this tick's overlaps.
        void PublishSensorOverlaps(Scene& scene, const PhysicsWorld& world)
        {
            vector<Entity> current;
            for (const Entity owner : world.GetNative().SensorEntities)
            {
                const Sensor* sensor = scene.IsAlive(owner) ? scene.TryGet<Sensor>(owner) : nullptr;
                auto* overlaps = sensor != nullptr ? scene.TryGet<SensorOverlaps>(owner) : nullptr;
                if (overlaps == nullptr)
                {
                    continue;
                }
                world.GetSensorOverlaps(owner, sensor->Layers, current);

                overlaps->Entered.clear();
                overlaps->Exited.clear();
                // Both sides are sorted by entity slot, so the deltas are two linear scans.
                std::ranges::set_difference(
                    current, overlaps->Current, std::back_inserter(overlaps->Entered),
                    [](const Entity a, const Entity b) { return a.Index < b.Index; });
                std::ranges::set_difference(
                    overlaps->Current, current, std::back_inserter(overlaps->Exited),
                    [](const Entity a, const Entity b) { return a.Index < b.Index; });
                overlaps->Current = current;
            }
        }
    }

    void Detail::NotePhysicsComponentRemoved(const Scene& scene, const Entity entity)
    {
        if (PhysicsWorld* world = scene.GetPhysicsWorld())
        {
            world->GetNative().Pending.push_back(entity);
        }
    }

    PhysicsWorld& EnsurePhysicsWorld(Scene& scene)
    {
        if (PhysicsWorld* installed = scene.GetPhysicsWorld())
        {
            return *installed;
        }
        const auto* authored = scene.TryGetFirst<PhysicsSettings>();
        const PhysicsSettings settings = authored != nullptr ? *authored : PhysicsSettings{};
        Unique<PhysicsWorld> world = PhysicsWorld::Create(ToPhysicsWorldInfo(settings));
        world->SetDebugDrawEnabled(settings.DebugDraw);
        PhysicsWorld& created = *world;
        scene.SetPhysicsWorld(std::move(world));
        return created;
    }

    void GatherGravitySources(const Scene& scene, vector<GravitySourceInstance>& out)
    {
        out.clear();
        for (auto [entity, source] : scene.View<GravitySource>())
        {
            vec3 position(0.0f);
            quat rotation(1.0f, 0.0f, 0.0f, 0.0f);
            if (const auto* transform = scene.TryGet<Transform>(entity))
            {
                position = transform->Position;
                rotation = transform->Rotation;
            }

            Region bounds = source.Bounds;
            bounds.Center = position + rotation * source.Bounds.Center;
            bounds.Orientation = rotation * source.Bounds.Orientation;

            out.emplace_back(GravitySourceInstance{
                .Kind = source.Kind,
                .Direction = rotation * source.Direction,
                .Origin = position,
                .Magnitude = source.Magnitude,
                .InnerRadius = source.InnerRadius,
                .OuterRadius = source.OuterRadius,
                .Bounds = bounds,
                .Priority = source.Priority,
                .BlendWidth = source.BlendWidth,
            });
        }
    }

    void StepPhysics(Scene& scene, const f32 delta)
    {
        PhysicsWorld* world = scene.GetPhysicsWorld();
        if (world == nullptr)
        {
            return;
        }
        PhysicsWorld::Native& native = world->GetNative();
        PhysicsStepStats stats;

        // The components are the authority and the bodies are their shadow: bring the bodies whose
        // components changed into line. The world's own body calls queue nothing meanwhile.
        native.Reconciling = true;
        Reconcile(scene, *world, stats);
        native.Reconciling = false;

        const Scene& readScene = scene;
        ReconcileConstraints(readScene, *world);

        // Gravity is a field, not a world constant: gather the scene's sources once per tick and
        // install them, so the step listener evaluates them per dynamic body. An empty set clears
        // the field and leaves the world's uniform gravity in force.
        vector<GravitySourceInstance> gravity;
        GatherGravitySources(readScene, gravity);
        world->SetGravitySources(gravity);

        Push(scene, *world, delta, stats);

        {
            VE_PROFILE_SCOPE("Physics/Step");
            world->Step(delta);
        }
        stats.OptimizedBroadPhase = native.OptimizedLastStep;

        // Pull: PhysicsPose is written for every body the step moved, so a consumer reading it sees
        // this tick's result whether or not the Transform is bound to the solver.
        ReadBack(scene, *world, stats);

        PublishSensorOverlaps(scene, *world);
        native.Stats = stats;
    }

    void PhysicsSystem::OnStart(Scene& scene, const SystemContext&)
    {
        EnsurePhysicsWorld(scene);
    }

    void PhysicsSystem::OnUpdate(Scene& scene, const f32 delta, const SystemContext& context)
    {
        // A reconciliation replay re-runs the whole Sim phase. A scene with no predicted body does
        // not participate in rollback: the solver's own state — dynamic velocities, the contact
        // cache, sleep — is not restored, so stepping here would advance the physics clock against
        // state that was never rewound and drift it from the sim tick permanently. Such a scene is
        // gated out.
        //
        // A scene predicting a character *does* roll back. The character's capsule state is restored
        // from the per-tick save and its Transform from the authoritative record (the mover re-seats
        // the capsule onto it), and the world it collides against is either static or a kinematic
        // body a deterministic Sim system re-drives from the replayed tick. So the step runs during
        // replay, re-driving those kinematic bodies to the configuration the tick was actually in —
        // which is the correctness point: replaying a character against a moving surface's *current*
        // pose reconciles to a body that was never there. Client-authoritative *dynamic* bodies are
        // out of scope for prediction (server-authoritative, interpolated), so their replay drift is
        // not a concern this gate must guard.
        if (context.IsReplay && !HasPredictedBody(scene))
        {
            // The replay's Sim systems stamp past ticks, which the reconcile's change test cannot
            // see past, so the next live step walks every physics entity.
            if (PhysicsWorld* world = scene.GetPhysicsWorld())
            {
                world->GetNative().FullReconcile = true;
            }
            return;
        }

        StepPhysics(scene, delta);

        const PhysicsWorld* world = scene.GetPhysicsWorld();
        if (world != nullptr && world->IsDebugDrawEnabled() && context.Debug != nullptr)
        {
            world->DrawDebug(*context.Debug);
        }
    }

    void PhysicsSystem::OnStop(Scene& scene, const SystemContext&)
    {
        if (PhysicsWorld* world = scene.GetPhysicsWorld())
        {
            world->DestroyAllBodies();
        }
    }
}
