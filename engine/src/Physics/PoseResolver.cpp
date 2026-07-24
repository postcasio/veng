#include <Veng/Physics/PoseResolver.h>

#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Transforms.h>

#include <glm/gtc/quaternion.hpp>

namespace Veng
{
    namespace
    {
        /// @brief Extracts a rotation from a world matrix, tolerating a uniform scale on its basis.
        [[nodiscard]] quat RotationOf(const mat4& world)
        {
            return glm::quat_cast(mat3(glm::normalize(vec3(world[0])),
                                       glm::normalize(vec3(world[1])),
                                       glm::normalize(vec3(world[2]))));
        }
    }

    PhysicsPose DefaultResolvePhysicsPose(const Scene& scene, const Entity entity,
                                          const mat4& localOffset)
    {
        const mat4 world = WorldMatrix(scene, entity) * localOffset;
        return PhysicsPose{.Position = dvec3(vec3(world[3])), .Rotation = RotationOf(world)};
    }

    void DefaultPlaceAtPhysicsPose(Scene& scene, const Entity entity, const PhysicsPose& pose)
    {
        Transform& transform = scene.Has<Transform>(entity) ? scene.Get<Transform>(entity)
                                                            : scene.Add<Transform>(entity);
        transform.Position = vec3(pose.Position);
        transform.Rotation = pose.Rotation;
        transform.Scale = vec3(1.0f);
    }

    PhysicsPose ResolvePhysicsPose(const Scene& scene, const Entity entity, const mat4& localOffset)
    {
        const PhysicsPoseResolver* resolver = scene.GetPhysicsPoseResolver();
        if (resolver != nullptr && resolver->Resolve)
        {
            return resolver->Resolve(scene, entity, localOffset);
        }
        return DefaultResolvePhysicsPose(scene, entity, localOffset);
    }

    void PlaceAtPhysicsPose(Scene& scene, const Entity entity, const PhysicsPose& pose)
    {
        const PhysicsPoseResolver* resolver = scene.GetPhysicsPoseResolver();
        if (resolver != nullptr && resolver->Place)
        {
            resolver->Place(scene, entity, pose);
            return;
        }
        DefaultPlaceAtPhysicsPose(scene, entity, pose);
    }
}
