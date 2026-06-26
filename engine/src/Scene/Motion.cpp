#include <Veng/Scene/Motion.h>

#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include <glm/gtc/quaternion.hpp>

namespace Veng
{
    void IntegrateConstantMotion(Transform& transform, const ConstantMotion& motion,
                                 const f32 delta)
    {
        const bool local = motion.Space == MotionSpace::Local;

        // Local drift moves along the entity's own axes; world drift moves in the parent
        // frame (Position is parent-space, so a world velocity adds directly).
        const vec3 linear =
            local ? transform.Rotation * motion.LinearVelocity : motion.LinearVelocity;
        transform.Position += linear * delta;

        // Angular velocity is an axis-angle vector: |w| is radians/sec, w/|w| the axis. Skip
        // a zero vector (no axis to normalize). Local spin post-multiplies (about the entity's
        // own axis); world spin pre-multiplies (about the parent axis).
        const f32 speed = glm::length(motion.AngularVelocity);
        if (speed > 0.0f)
        {
            const vec3 axis = motion.AngularVelocity / speed;
            const quat step = glm::angleAxis(speed * delta, axis);
            transform.Rotation =
                glm::normalize(local ? transform.Rotation * step : step * transform.Rotation);
        }
    }

    void ConstantMotionSystem::OnUpdate(Scene& scene, const f32 delta, const SystemContext&)
    {
        scene.Each<Transform, ConstantMotion>(
            [delta](const Entity, Transform& transform, ConstantMotion& motion)
            { IntegrateConstantMotion(transform, motion, delta); });
    }
}
