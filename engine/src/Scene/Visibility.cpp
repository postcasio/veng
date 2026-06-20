#include <Veng/Scene/Visibility.h>

#include <Veng/Asset/Mesh.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Transforms.h>

namespace Veng
{
    void GatherMeshes(const Scene& scene, vector<VisibleMesh>& out, AABB& outBounds)
    {
        // ComputeWorldMatrices uses Transform pool dense order, matching DensePtr below,
        // so worldMatrices[i] is the world matrix for dense[i].
        vector<mat4> worldMatrices;
        ComputeWorldMatrices(scene, worldMatrices);

        const TypeId transformId = scene.m_Registry->IdOf<Transform>();
        const Entity* dense = scene.DensePtr(transformId);
        const usize count = scene.PoolCount(transformId);

        out.clear();
        outBounds = AABB::Empty();
        for (usize i = 0; i < count; ++i)
        {
            const MeshRenderer* renderer = scene.TryGet<MeshRenderer>(dense[i]);
            if (renderer == nullptr || !renderer->Mesh.IsLoaded())
                continue;

            const AABB worldBounds = renderer->Mesh->GetBounds().Transformed(worldMatrices[i]);
            out.push_back(VisibleMesh{
                .Owner = dense[i],
                .World = worldMatrices[i],
                .WorldBounds = worldBounds,
                .Mesh = renderer->Mesh.Get(),
            });
            outBounds.Expand(worldBounds);
        }
    }
}
