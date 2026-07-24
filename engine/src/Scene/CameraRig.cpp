#include <Veng/Scene/CameraRig.h>

#include <Veng/Asset/Mesh.h>
#include <Veng/Physics/CharacterController.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Sockets.h>
#include <Veng/Scene/Transforms.h>

#include <glm/gtc/constants.hpp>
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

    quat LookRotation(const CameraLook& look)
    {
        const f32 limit = glm::max(look.PitchLimit, 0.0f);
        const f32 pitch = glm::clamp(look.Pitch, -limit, limit);
        return glm::angleAxis(look.Yaw, vec3(0.0f, 1.0f, 0.0f)) *
               glm::angleAxis(pitch, vec3(1.0f, 0.0f, 0.0f));
    }

    Transform OrbitCamera(const CameraOrbit& orbit, const Transform& current, const f32 delta)
    {
        // Glide the focus toward its target, frame-rate-independent like FollowCamera; a
        // non-positive damping snaps.
        vec3 focus = orbit.FocusTarget;
        if (orbit.FocusDamping > 0.0f)
        {
            const f32 alpha = 1.0f - glm::exp(-orbit.FocusDamping * delta);
            focus = glm::mix(orbit.Focus, orbit.FocusTarget, alpha);
        }

        // Clamp the distance into its band, and build the orbit pose with the same y-up
        // yaw/pitch composition LookRotation applies — reusing its pitch clamp keeps the eye off
        // the pole where the look-at up vector collapses.
        const f32 minDistance = glm::max(orbit.MinDistance, 0.0f);
        const f32 maxDistance = glm::max(orbit.MaxDistance, minDistance);
        const f32 distance = glm::clamp(orbit.Distance, minDistance, maxDistance);
        const quat pose = LookRotation(
            CameraLook{.Yaw = orbit.Yaw, .Pitch = orbit.Pitch, .PitchLimit = orbit.PitchLimit});

        // Place the eye a distance out along the pose's +Z (opposite the look direction) and
        // orient it back at the focus.
        Transform result = current;
        result.Position = focus + pose * vec3(0.0f, 0.0f, distance);
        result.Rotation =
            glm::quatLookAt(glm::normalize(focus - result.Position), vec3(0.0f, 1.0f, 0.0f));
        return result;
    }

    CameraBasis FirstPersonBasis(const vec3 characterUp, const vec3 targetForward, const f32 yaw,
                                 const f32 pitch, const f32 minPitch, const f32 maxPitch)
    {
        const vec3 up = glm::normalize(characterUp);

        // The forward reference is the target's forward flattened into the plane perpendicular to
        // up. When the target looks straight along its own up the projection collapses, so fall
        // back to any axis not parallel to up — the yaw then measures from that arbitrary heading.
        vec3 flat = targetForward - glm::dot(targetForward, up) * up;
        if (glm::dot(flat, flat) < 1e-8f)
        {
            const vec3 seed =
                std::abs(up.z) < 0.9f ? vec3(0.0f, 0.0f, 1.0f) : vec3(1.0f, 0.0f, 0.0f);
            flat = seed - glm::dot(seed, up) * up;
        }
        flat = glm::normalize(flat);

        // Yaw about the character's up keeps the heading in the up-plane; right is then
        // perpendicular to up by construction, so the horizon stays level for any up — the whole
        // point of building the basis against the character's up rather than the world's.
        const vec3 yawed = glm::angleAxis(yaw, up) * flat;
        const vec3 planarForward = glm::normalize(yawed - glm::dot(yawed, up) * up);
        const vec3 right = glm::normalize(glm::cross(planarForward, up));

        // Pitch about right, clamped to the rig's band so the forward never reaches the up pole
        // where the level-horizon orientation would be undefined.
        const f32 clamped = glm::clamp(pitch, glm::min(minPitch, maxPitch), maxPitch);
        const vec3 forward = glm::normalize(glm::angleAxis(clamped, right) * planarForward);
        const vec3 camUp = glm::normalize(glm::cross(right, forward));
        return CameraBasis{.Right = right, .Up = camUp, .Forward = forward};
    }

    Transform FirstPersonCamera(const vec3 eyeAnchor, const vec3 characterUp,
                                const vec3 targetForward, const CameraLook& look,
                                const FirstPersonRig& rig, const f32 bobPhase)
    {
        const CameraBasis basis = FirstPersonBasis(characterUp, targetForward, look.Yaw, look.Pitch,
                                                   rig.MinPitch, rig.MaxPitch);

        // View bob: a vertical oscillation at the step phase and a lateral one at half of it (a
        // walk sways side to side once per stride, bounces twice), both scaled by the amplitude.
        // Zero amplitude leaves the eye exactly at the anchor.
        const vec3 bob = basis.Up * (std::sin(bobPhase) * rig.BobAmplitude) +
                         basis.Right * (std::cos(bobPhase * 0.5f) * rig.BobAmplitude * 0.5f);

        Transform result;
        result.Position = eyeAnchor + bob;
        result.Rotation = glm::quat_cast(mat3(basis.Right, basis.Up, -basis.Forward));
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

        scene.Each<Transform, CameraOrbit>(
            [delta](const Entity entity, Transform& transform, CameraOrbit& orbit)
            {
                transform = OrbitCamera(orbit, transform, delta);

                // Persist the glided focus so the ease progresses across ticks (mirroring the
                // glide OrbitCamera applied for this tick's pose); a non-positive damping snaps.
                if (orbit.FocusDamping > 0.0f)
                {
                    const f32 alpha = 1.0f - glm::exp(-orbit.FocusDamping * delta);
                    orbit.Focus = glm::mix(orbit.Focus, orbit.FocusTarget, alpha);
                }
                else
                {
                    orbit.Focus = orbit.FocusTarget;
                }
            });

        scene.Each<Transform, FirstPersonRig>(
            [&scene, delta](const Entity entity, Transform& transform, FirstPersonRig& rig)
            {
                if (rig.Target == Entity::Null || !scene.IsAlive(rig.Target) ||
                    !scene.Has<Transform>(rig.Target))
                {
                    return;
                }

                const mat4 targetWorld = WorldMatrix(scene, rig.Target);
                const vec3 targetPosition = vec3(targetWorld[3]);
                const quat targetRotation = glm::quat_cast(mat3(targetWorld));

                // The up is the target's resolved up when it carries CharacterState, else its own
                // transform up — so the rig works before the first character tick and on a target
                // that is not a character.
                vec3 up = targetRotation * vec3(0.0f, 1.0f, 0.0f);
                f32 planarSpeed = 0.0f;
                if (const CharacterState* state = scene.TryGet<CharacterState>(rig.Target))
                {
                    up = state->Up;
                    planarSpeed = state->PlanarSpeed;
                }

                // Resolve the eye anchor: a named socket on the target's mesh when it resolves,
                // else the local EyeOffset. Either way EyeOffset is applied in the anchor's frame.
                vec3 eyeAnchor = targetPosition + targetRotation * rig.EyeOffset;
                if (!rig.EyeSocket.empty())
                {
                    if (const MeshSocket* socket = FindMeshSocket(scene, rig.Target, rig.EyeSocket))
                    {
                        const mat4 socketLocal = glm::translate(mat4(1.0f), socket->Position) *
                                                 glm::mat4_cast(socket->Rotation) *
                                                 glm::scale(mat4(1.0f), socket->Scale);
                        const mat4 socketWorld = targetWorld * socketLocal;
                        eyeAnchor = vec3(socketWorld[3]) + mat3(socketWorld) * rig.EyeOffset;
                    }
                }

                // Advance the bob phase by the distance walked this tick (speed × time × cycles per
                // metre, in radians), so the bob tracks gait rather than wall clock and holds still
                // when the character does.
                rig.BobPhase += planarSpeed * rig.BobFrequency * glm::two_pi<f32>() * delta;

                const vec3 targetForward = targetRotation * vec3(0.0f, 0.0f, -1.0f);
                auto* heading = scene.TryGet<CameraLook>(entity);
                const CameraLook look = heading != nullptr ? *heading : CameraLook{};
                transform =
                    FirstPersonCamera(eyeAnchor, up, targetForward, look, rig, rig.BobPhase);

                // Store the clamped pitch back so accumulated look input never winds past the band.
                if (heading != nullptr)
                {
                    heading->Pitch = glm::clamp(heading->Pitch,
                                                glm::min(rig.MinPitch, rig.MaxPitch), rig.MaxPitch);
                }
            });

        scene.Each<Transform, CameraLook>(
            [&scene](const Entity entity, Transform& transform, CameraLook& look)
            {
                // A first-person rig drives its own entity's pose and heading; leave it to that arm.
                if (scene.Has<FirstPersonRig>(entity))
                {
                    return;
                }

                // Store the clamp back so accumulated look input never winds past the limit.
                look.Pitch = glm::clamp(look.Pitch, -glm::max(look.PitchLimit, 0.0f),
                                        glm::max(look.PitchLimit, 0.0f));
                transform.Rotation = LookRotation(look);
            });
    }
}
