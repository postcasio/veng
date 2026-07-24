#include <Veng/Physics/PhysicsSystem.h>

#include <Veng/Physics/Components.h>
#include <Veng/Physics/PhysicsWorld.h>
#include <Veng/Renderer/DebugDraw.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

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
        for (auto [entity, body, collider] : readScene.View<RigidBody, Collider>())
        {
            simulated.emplace_back(Simulated{.Owner = entity, .Body = body, .Shape = collider});
            if (!readScene.Has<PhysicsPose>(entity))
            {
                needPose.emplace_back(entity);
            }
        }
        for (const Entity entity : needPose)
        {
            scene.Add<PhysicsPose>(entity);
        }

        // The components are the authority and the bodies are their shadow: bring every physics
        // entity's body into line, then destroy the bodies whose entity no longer has one.
        for (const Simulated& entry : simulated)
        {
            world->CreateBody(entry.Owner, entry.Body, entry.Shape,
                              ReadPose(readScene, entry.Owner, entry.Body));
        }

        vector<Entity> resident;
        world->GetBodyEntities(resident);
        for (const Entity entity : resident)
        {
            if (!scene.IsAlive(entity) || !readScene.Has<RigidBody>(entity) ||
                !readScene.Has<Collider>(entity))
            {
                world->DestroyBody(entity);
            }
        }

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
    }

    void PhysicsSystem::OnUpdate(Scene& scene, const f32 delta, const SystemContext& context)
    {
        // A reconciliation replay re-runs the whole Sim phase, but the solver's own state —
        // velocities, the contact cache, sleep — is not in the prediction history and is therefore
        // never restored. Stepping here would advance the physics clock once per replayed tick
        // against state that was never rewound, and the drift from the sim tick is permanent.
        if (context.IsReplay)
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
