#include <Veng/Scene/Movement.h>

#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include <glm/gtc/quaternion.hpp>

namespace Veng
{
    void IntegrateMovement(Transform& transform, const Intent& intent, const Mover& mover,
                           const f32 delta)
    {
        // Move is pawn-local: rotate it into the transform's current orientation before
        // translating, so "forward" follows where the pawn faces.
        const vec3 worldMove = transform.Rotation * intent.Move;
        transform.Position += worldMove * mover.MoveSpeed * delta;

        // Yaw about the world up keeps the pawn upright; pitch about the pawn's local
        // right tilts the facing. Applying yaw on the left and pitch on the right keeps
        // them independent of accumulated roll.
        const quat yaw =
            glm::angleAxis(intent.Look.x * mover.TurnSpeed * delta, vec3(0.0f, 1.0f, 0.0f));
        const quat pitch =
            glm::angleAxis(intent.Look.y * mover.TurnSpeed * delta, vec3(1.0f, 0.0f, 0.0f));
        transform.Rotation = glm::normalize(yaw * transform.Rotation * pitch);
    }

    void MovementSystem::OnUpdate(Scene& scene, const f32 delta, const SystemContext&)
    {
        // A pawn without a Mover moves at the component's defaults.
        static constexpr Mover DefaultMover{};

        scene.Each<Transform, Intent>(
            [&scene, delta](const Entity entity, Transform& transform, Intent& intent)
            {
                const Mover* mover = scene.TryGet<Mover>(entity);
                IntegrateMovement(transform, intent, mover != nullptr ? *mover : DefaultMover,
                                  delta);
            });
    }
}
