#include <Veng/Scene/CameraRig.h>

#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Transforms.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>
#include <glm/gtx/quaternion.hpp>

#include <cmath>

namespace Veng
{
    Transform FollowCamera(const Transform& current, const mat4& targetWorld,
                           const CameraFollow& follow, const f32 delta)
    {
        const vec3 targetPosition = vec3(targetWorld[3]);
        const quat targetRotation = glm::quat_cast(mat3(targetWorld));

        // The offset trails the target by its yaw only: a target that turns swings the
        // camera around it, but a target that pitches or rolls never tips the camera off
        // the height the offset's +Y places it at (a pawn looking down keeps the camera
        // above). The camera then looks back at the target.
        const vec3 facing = targetRotation * vec3(0.0f, 0.0f, -1.0f);
        const f32 yaw = std::atan2(-facing.x, -facing.z);
        const quat yawRotation = glm::angleAxis(yaw, vec3(0.0f, 1.0f, 0.0f));

        // Orbit the offset up and down around the target by the follow Pitch, about the
        // yaw-rotated right axis — so a look-up/down tilts the camera around the target
        // rather than rotating the target itself.
        const quat pitchRotation =
            glm::angleAxis(follow.Pitch, yawRotation * vec3(1.0f, 0.0f, 0.0f));
        const vec3 goalPosition = targetPosition + pitchRotation * (yawRotation * follow.Offset);
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
