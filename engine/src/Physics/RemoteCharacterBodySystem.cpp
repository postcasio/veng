#include <Veng/Physics/RemoteCharacterBodySystem.h>

#include <Veng/Physics/CharacterController.h>
#include <Veng/Physics/Components.h>
#include <Veng/Physics/PhysicsWorld.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include <algorithm>

namespace Veng
{
    namespace
    {
        /// @brief The capsule Collider matching a CharacterController's own feet-origin capsule.
        ///
        /// The character's capsule is centred on its middle and lifted so the feet sit at the
        /// entity origin (the pose a character Transform names), so the proxy collider is the same
        /// capsule at the same offset — the local character then slides against exactly the shape it
        /// would collide with were the remote character solid.
        /// @param controller  The remote character's authored shape.
        /// @return The collider describing its capsule.
        [[nodiscard]] Collider ProxyCollider(const CharacterController& controller)
        {
            const f32 halfCylinder =
                std::max(1.0e-3f, controller.Height * 0.5f - controller.Radius);
            return Collider{
                .Shape = ColliderShape::Capsule,
                .Extents = vec3(controller.Radius, halfCylinder, 0.0f),
                .Offset = vec3(0.0f, controller.Height * 0.5f, 0.0f),
            };
        }
    }

    void RemoteCharacterBodySystem::OnUpdate(Scene& scene, f32 /*delta*/,
                                             const SystemContext& context)
    {
        if (scene.GetPhysicsWorld() == nullptr)
        {
            return;
        }

        // Gather through the const view: adding or removing a RigidBody/Collider is a structural
        // change, so the pass that decides cannot be the pass that mutates.
        const Scene& readScene = scene;
        vector<Entity> grow;
        vector<Entity> drop;
        for (auto [entity, transform, controller] :
             readScene.View<Transform, CharacterController>())
        {
            (void)transform;
            (void)controller;
            // A character this peer simulates gets a capsule from the mover, not a proxy body; only a
            // remote one needs a body to be blocked by.
            const bool remote = !HasAuthority(context, scene, entity);
            const bool hasProxy = readScene.Has<RigidBody>(entity);
            if (remote && !hasProxy)
            {
                grow.emplace_back(entity);
            }
            else if (!remote && hasProxy)
            {
                drop.emplace_back(entity);
            }
        }

        for (const Entity entity : grow)
        {
            const CharacterController& controller = scene.Get<CharacterController>(entity);
            scene.Add<RigidBody>(
                entity, RigidBody{.Motion = MotionType::Kinematic, .Layer = PhysicsLayer::Moving});
            scene.Add<Collider>(entity, ProxyCollider(controller));
        }
        for (const Entity entity : drop)
        {
            (void)scene.Remove<Collider>(entity);
            (void)scene.Remove<RigidBody>(entity);
        }
    }
}
