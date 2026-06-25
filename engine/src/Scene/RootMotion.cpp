#include <Veng/Scene/RootMotion.h>

#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

namespace Veng
{
    void RootMotionDriveSystem::OnUpdate(Scene& scene, f32 /*delta*/,
                                         const SystemContext& /*context*/)
    {
        scene.Each<Transform, RootMotionDelta>(
            [](const Entity /*entity*/, Transform& transform, RootMotionDelta& motion)
            {
                // The delta is model-local (the pawn's own forward/right/up); orient and scale it
                // into the entity's frame before adding, as MovementSystem does for a local move.
                transform.Position += transform.Rotation * (transform.Scale * motion.Translation);
                motion.Translation = vec3(0.0f);
            });
    }
}
