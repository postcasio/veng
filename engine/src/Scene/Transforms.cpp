#include <Veng/Scene/Transforms.h>

#include <Veng/Assert.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Scene/Scene.h>

#include <glm/gtc/matrix_transform.hpp>

namespace Veng
{
    mat4 LocalMatrix(const Transform& transform)
    {
        const mat4 translation = glm::translate(mat4(1.0f), transform.Position);
        const mat4 rotation = glm::mat4_cast(transform.Rotation);
        const mat4 scale = glm::scale(mat4(1.0f), transform.Scale);
        return translation * rotation * scale;
    }

    mat4 WorldMatrix(const Scene& scene, Entity entity)
    {
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
        const TypeId id = scene.m_Registry->IdOf<Transform>();
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
        // ComputeWorldMatrices uses Transform pool dense order, matching DensePtr below,
        // so worldMatrices[i] is the world matrix for dense[i].
        vector<mat4> worldMatrices;
        ComputeWorldMatrices(scene, worldMatrices);

        const TypeId transformId = scene.m_Registry->IdOf<Transform>();
        const Entity* dense = scene.DensePtr(transformId);
        const usize count = scene.PoolCount(transformId);

        AABB bounds = AABB::Empty();
        for (usize i = 0; i < count; ++i)
        {
            const auto* renderer = scene.TryGet<MeshRenderer>(dense[i]);
            if (renderer == nullptr || !renderer->Mesh.IsLoaded())
            {
                continue;
            }

            bounds.Expand(renderer->Mesh->GetBounds().Transformed(worldMatrices[i]));
        }
        return bounds;
    }
}
