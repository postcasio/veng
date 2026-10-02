#include <Veng/Scene/Transforms.h>

#include <Veng/Assert.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Diagnostics/Profiler.h>
#include <Veng/Scene/Scene.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Veng
{
    mat4 LocalMatrix(const Transform& transform)
    {
        // T * R * S written out: the rotation's columns scaled by the scale, the position as the
        // translation column. Equal to multiplying the three matrices, since every term those
        // products add beyond these is a zero.
        const mat3 rotation = glm::mat3_cast(transform.Rotation);
        mat4 local(1.0f);
        local[0] = vec4(rotation[0] * transform.Scale.x, 0.0f);
        local[1] = vec4(rotation[1] * transform.Scale.y, 0.0f);
        local[2] = vec4(rotation[2] * transform.Scale.z, 0.0f);
        local[3] = vec4(transform.Position, 1.0f);
        return local;
    }

    Transform InterpolateTransform(const Transform& from, const Transform& to, const f32 alpha)
    {
        return Transform{
            .Position = glm::mix(from.Position, to.Position, alpha),
            .Rotation = glm::slerp(from.Rotation, to.Rotation, alpha),
            .Scale = glm::mix(from.Scale, to.Scale, alpha),
        };
    }

    mat4 WorldMatrix(const Scene& scene, Entity entity)
    {
        if (const mat4* world = scene.FindWorldMatrix(entity))
        {
            return *world;
        }

        // Walk the Hierarchy chain entity → root, collecting it so the cycle/dead-
        // entity checks can run before composing. A revisited entity is a cycle;
        // a parent link pointing at a dead entity is a dangling link — both API
        // misuse.
        vector<Entity> chain;
        Entity current = entity;
        while (!current.IsNull())
        {
            VE_ASSERT(scene.IsAlive(current),
                      "WorldMatrix: Hierarchy references a dead or stale entity");

            for (const Entity seen : chain)
            {
                VE_ASSERT(seen != current, "WorldMatrix: Hierarchy chain forms a cycle");
            }
            chain.push_back(current);

            if (const auto* hierarchy = scene.TryGet<Hierarchy>(current))
            {
                current = hierarchy->Parent;
            }
            else
            {
                current = Entity::Null;
            }
        }

        // chain is entity-first (collected above), so walk root → entity in reverse.
        mat4 world(1.0f);
        for (usize i = chain.size(); i-- > 0;)
        {
            if (const auto* transform = scene.TryGet<Transform>(chain[i]))
            {
                world = world * LocalMatrix(*transform);
            }
        }
        return world;
    }

    void ComputeWorldMatrices(const Scene& scene, vector<mat4>& out)
    {
        scene.UpdateWorldTransforms();

        const TypeId id = TypeIdOf<Transform>();
        const usize count = scene.PoolCount(id);
        const Entity* dense = scene.DensePtr(id);

        out.clear();
        out.reserve(count);
        for (usize i = 0; i < count; ++i)
        {
            out.push_back(WorldMatrix(scene, dense[i]));
        }
    }

    AABB SceneBounds(const Scene& scene)
    {
        scene.UpdateWorldTransforms();

        AABB bounds = AABB::Empty();
        for (auto [entity, renderer] : scene.View<MeshRenderer>())
        {
            if (!renderer.Visible || !renderer.Mesh.IsLoaded() || !scene.Has<Transform>(entity))
            {
                continue;
            }

            bounds.Expand(renderer.Mesh->GetBounds().Transformed(WorldMatrix(scene, entity)));
        }
        return bounds;
    }
}
