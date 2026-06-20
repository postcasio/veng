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
        // Walk the Parent chain entity → root, collecting it so the cycle/dead-
        // entity checks can run before composing. A revisited entity is a cycle;
        // a Parent pointing at a dead entity is a dangling link — both API misuse.
        vector<Entity> chain;
        Entity current = entity;
        while (!current.IsNull())
        {
            VE_ASSERT(scene.IsAlive(current),
                      "WorldMatrix: Parent references a dead or stale entity");

            for (const Entity seen : chain)
            {
                VE_ASSERT(seen != current, "WorldMatrix: Parent chain forms a cycle");
            }
            chain.push_back(current);

            if (const Parent* parent = scene.TryGet<Parent>(current))
            {
                current = parent->Value;
            }
            else
            {
                current = Entity::Null;
            }
        }

        // Compose root → entity: world = parent.world * local. chain is
        // entity-first, so walk it in reverse.
        mat4 world(1.0f);
        for (usize i = chain.size(); i-- > 0;)
        {
            if (const Transform* transform = scene.TryGet<Transform>(chain[i]))
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
        // One amortized pass for every Transform-bearing entity's world matrix,
        // in the Transform pool's dense order — the same order DensePtr walks
        // below, so a Transform entity's world matrix is out[i].
        vector<mat4> worldMatrices;
        ComputeWorldMatrices(scene, worldMatrices);

        const TypeId transformId = scene.m_Registry->IdOf<Transform>();
        const Entity* dense = scene.DensePtr(transformId);
        const usize count = scene.PoolCount(transformId);

        AABB bounds = AABB::Empty();
        for (usize i = 0; i < count; ++i)
        {
            const MeshRenderer* renderer = scene.TryGet<MeshRenderer>(dense[i]);
            if (renderer == nullptr || !renderer->Mesh.IsLoaded())
                continue;

            bounds.Expand(renderer->Mesh->GetBounds().Transformed(worldMatrices[i]));
        }
        return bounds;
    }
}
