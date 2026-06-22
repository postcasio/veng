#include <Veng/Scene/CameraRig.h>

#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Transforms.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

namespace Veng
{
    Transform FollowCamera(const Transform& current, const mat4& targetWorld,
                           const CameraFollow& follow, const f32 delta)
    {
        const vec3 targetPosition = vec3(targetWorld[3]);
        const quat targetRotation = glm::quat_cast(mat3(targetWorld));

        // Offset is in the target's local frame, so a target that turns swings the camera
        // with it; the camera then looks back at the target.
        const vec3 goalPosition = targetPosition + targetRotation * follow.Offset;
        const quat goalRotation =
            glm::quatLookAt(glm::normalize(targetPosition - goalPosition), vec3(0.0f, 1.0f, 0.0f));

        Transform result = current;
        if (follow.Damping > 0.0f)
        {
            // Frame-rate-independent exponential smoothing toward the goal.
            const f32 alpha = 1.0f - glm::exp(-follow.Damping * delta);
            result.Position = glm::mix(current.Position, goalPosition, alpha);
            result.Rotation = glm::normalize(glm::slerp(current.Rotation, goalRotation, alpha));
        }
        else
        {
            result.Position = goalPosition;
            result.Rotation = goalRotation;
        }
        return result;
    }

    void CameraRigSystem::OnUpdate(Scene& scene, const f32 delta, const SystemContext&)
    {
        scene.Each<Transform, CameraFollow>(
            [&scene, delta](const Entity entity, Transform& transform, CameraFollow& follow)
            {
                if (follow.Target == Entity::Null || !scene.IsAlive(follow.Target) ||
                    !scene.Has<Transform>(follow.Target))
                {
                    return;
                }

                const mat4 targetWorld = WorldMatrix(scene, follow.Target);
                transform = FollowCamera(transform, targetWorld, follow, delta);
            });
    }
}
