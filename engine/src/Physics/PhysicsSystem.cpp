#include <Veng/Physics/PhysicsSystem.h>

#include <Veng/Physics/Components.h>
#include <Veng/Physics/PhysicsWorld.h>
#include <Veng/Renderer/DebugDraw.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include <algorithm>

namespace Veng
{
    namespace
    {
        /// @brief One physics entity, snapshotted so the passes below need no repeated lookups.
        struct Simulated
        {
            /// @brief The entity owning the body.
            Entity Owner;
            /// @brief Its RigidBody settings this tick.
            RigidBody Body;
            /// @brief Its Collider settings this tick.
            Collider Shape;
            /// @brief Whether the entity carries a Sensor.
            bool IsSensor = false;
        };

        /// @brief Reads an entity's authoritative pose for this tick.
        ///
        /// With SyncTransform set the Transform is the pose (world space — a physics entity is a
        /// scene-graph root); cleared, the entity's PhysicsPose is the only channel.
        /// @param scene   The scene the entity lives in.
        /// @param entity  The entity to read.
        /// @param body    The entity's RigidBody, for the SyncTransform flag.
        /// @return The pose the solver should adopt.
        [[nodiscard]] PhysicsPose ReadPose(const Scene& scene, const Entity entity,
                                           const RigidBody& body)
        {
            if (body.SyncTransform)
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

        /// @brief Publishes each sensor's overlap set and this tick's enter/exit deltas.
        ///
        /// State a system drains, not an event it subscribes to: the deltas are computed here,
        /// after the step, so no gameplay code runs inside the solver.
        /// @param scene      The scene whose Sensor entities are published to.
        /// @param world      The world holding this tick's overlaps.
        /// @param simulated  The entities the step reconciled, in scene order.
        void PublishSensorOverlaps(Scene& scene, const PhysicsWorld& world,
                                   const vector<Simulated>& simulated)
        {
            vector<Entity> current;
            for (const Simulated& entry : simulated)
            {
                if (!entry.IsSensor)
                {
                    continue;
                }
                const Sensor& sensor = scene.Get<Sensor>(entry.Owner);
                world.GetSensorOverlaps(entry.Owner, sensor.Layers, current);

                auto& overlaps = scene.Get<SensorOverlaps>(entry.Owner);
                overlaps.Entered.clear();
                overlaps.Exited.clear();
                // Both sides are sorted by entity slot, so the deltas are two linear scans.
                std::ranges::set_difference(
                    current, overlaps.Current, std::back_inserter(overlaps.Entered),
                    [](const Entity a, const Entity b) { return a.Index < b.Index; });
                std::ranges::set_difference(
                    overlaps.Current, current, std::back_inserter(overlaps.Exited),
                    [](const Entity a, const Entity b) { return a.Index < b.Index; });
                overlaps.Current = current;
            }
        }
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

        // Gather through the const view: a structural change during iteration is illegal, and
        // adding a PhysicsPose is one, so the pass that finds them cannot be the pass that makes
        // them. The const view also leaves the spatial version alone.
        const Scene& readScene = scene;
        vector<Simulated> simulated;
        vector<Entity> needPose;
        vector<Entity> needOverlaps;
        for (auto [entity, body, collider] : readScene.View<RigidBody, Collider>())
        {
            if (!HasShape(collider))
            {
                continue;
            }
            const bool sensor = readScene.Has<Sensor>(entity);
            simulated.emplace_back(
                Simulated{.Owner = entity, .Body = body, .Shape = collider, .IsSensor = sensor});
            if (!readScene.Has<PhysicsPose>(entity))
            {
                needPose.emplace_back(entity);
            }
            if (sensor && !readScene.Has<SensorOverlaps>(entity))
            {
                needOverlaps.emplace_back(entity);
            }
        }
        for (const Entity entity : needPose)
        {
            scene.Add<PhysicsPose>(entity);
        }
        for (const Entity entity : needOverlaps)
        {
            scene.Add<SensorOverlaps>(entity);
        }

        // The components are the authority and the bodies are their shadow: bring every physics
        // entity's body into line, then destroy the bodies whose entity no longer has one.
        for (const Simulated& entry : simulated)
        {
            world->CreateBody(entry.Owner, entry.Body, entry.Shape,
                              ReadPose(readScene, entry.Owner, entry.Body), entry.IsSensor);
        }

        vector<Entity> resident;
        world->GetBodyEntities(resident);
        for (const Entity entity : resident)
        {
            const Collider* collider =
                scene.IsAlive(entity) ? readScene.TryGet<Collider>(entity) : nullptr;
            if (collider == nullptr || !readScene.Has<RigidBody>(entity) || !HasShape(*collider))
            {
                world->DestroyBody(entity);
            }
        }

        ReconcileConstraints(readScene, *world);

        // Gravity is a field, not a world constant: gather the scene's sources once per tick and
        // install them, so the step listener evaluates them per dynamic body. An empty set clears
        // the field and leaves the world's uniform gravity in force.
        vector<GravitySourceInstance> gravity;
        GatherGravitySources(readScene, gravity);
        world->SetGravitySources(gravity);

        // Push: a static body is placed outright, a kinematic one is swept toward its target so it
        // pushes what it meets instead of passing through. A dynamic body's pose is the solver's.
        for (const Simulated& entry : simulated)
        {
            if (entry.Body.Motion == MotionType::Dynamic)
            {
                continue;
            }
            const PhysicsPose target = ReadPose(readScene, entry.Owner, entry.Body);
            if (entry.Body.Motion == MotionType::Kinematic)
            {
                world->MoveKinematicBody(entry.Owner, target, delta);
            }
            else
            {
                world->SetBodyPose(entry.Owner, target);
            }
        }

        world->Step(delta);

        // Pull: PhysicsPose is always written, so a consumer reading it sees this tick's result
        // whether or not the Transform is bound to the solver.
        for (const Simulated& entry : simulated)
        {
            const optional<PhysicsPose> pose = world->GetBodyPose(entry.Owner);
            if (!pose.has_value())
            {
                continue;
            }
            scene.Get<PhysicsPose>(entry.Owner) = *pose;
            if (entry.Body.SyncTransform && entry.Body.Motion != MotionType::Static)
            {
                if (auto* transform = scene.TryGet<Transform>(entry.Owner))
                {
                    transform->Position = vec3(pose->Position);
                    transform->Rotation = pose->Rotation;
                }
            }
        }

        PublishSensorOverlaps(scene, *world, simulated);
    }

    void PhysicsSystem::OnUpdate(Scene& scene, const f32 delta, const SystemContext& context)
    {
        // A reconciliation replay re-runs the whole Sim phase. A scene with no predicted body does
        // not participate in rollback: the solver's own state — dynamic velocities, the contact
        // cache, sleep — is not restored, so stepping here would advance the physics clock against
        // state that was never rewound and drift it from the sim tick permanently. Such a scene is
        // gated out, exactly as before.
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
