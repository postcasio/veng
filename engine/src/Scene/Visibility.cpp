#include <Veng/Scene/Visibility.h>

#include <Veng/Asset/Mesh.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Transforms.h>

namespace Veng
{
    void GatherMeshes(const Scene& scene, vector<VisibleMesh>& out, AABB& outBounds)
    {
        // One amortized pass for every Transform-bearing entity's world matrix,
        // in the Transform pool's dense order — the same order DensePtr walks
        // below, so a Transform entity's world matrix is worldMatrices[i].
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
