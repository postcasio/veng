#include <Veng/Scene/Visibility.h>

#include <Veng/Asset/Mesh.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/RemoteInterpolationSystem.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Transforms.h>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtc/quaternion.hpp>

namespace Veng
{
    void GatherMeshes(const Scene& scene, vector<VisibleMesh>& out, AABB& outBounds,
                      const Entity exclude, const u32 layerMask)
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
            if (dense[i] == exclude)
            {
                continue;
            }

            const auto* renderer = scene.TryGet<MeshRenderer>(dense[i]);
            if (renderer == nullptr || !renderer->Visible || !renderer->Mesh.IsLoaded())
            {
                continue;
            }

            // The view's layer mask filters here, beside Visible, so nothing downstream re-tests it:
            // an off-mask renderer is absent from the candidate list and never widens outBounds.
            if (!RenderLayerInMask(layerMask, renderer->Layer))
            {
                continue;
            }

            // A predicted entity being visually smoothed after a reconciliation correction carries a
            // decaying render offset (position + rotation about its origin), applied only here — the
            // sim Transform stays authoritative; the render pose eases into it.
            mat4 world = worldMatrices[i];
            if (const auto* error = scene.TryGet<PredictionError>(dense[i]))
            {
                const vec3 origin = vec3(world[3]);
                world = glm::translate(mat4(1.0f), origin + error->Position) *
                        glm::mat4_cast(error->Rotation) * glm::translate(mat4(1.0f), -origin) *
                        world;
            }

            const AABB worldBounds = renderer->Mesh->GetBounds().Transformed(world);
            // A per-entity InstanceMaterials override replaces the mesh asset's shared list for this
            // entity; empty falls back to the asset's own, so a mesh with no override draws exactly
            // as before. The override is indexed by SubMesh::MaterialIndex like the asset's list, so
            // it is honoured only when it matches that list's length — a stale override left over a
            // swapped mesh falls back rather than risking an out-of-range index. Both spans stay
            // valid for this gather (the mesh is resident; the component is not structurally changed
            // mid-Execute).
            const std::span<const AssetHandle<MaterialInstance>> meshMaterials =
                renderer->Mesh.Get()->GetMaterials();
            const std::span<const AssetHandle<MaterialInstance>> materials =
                renderer->InstanceMaterials.size() == meshMaterials.size()
                    ? std::span<const AssetHandle<MaterialInstance>>(renderer->InstanceMaterials)
                    : meshMaterials;
            out.push_back(VisibleMesh{
                .Owner = dense[i],
                .World = world,
                .WorldBounds = worldBounds,
                .Mesh = renderer->Mesh.Get(),
                .Materials = materials,
                .CastsShadows = renderer->CastsShadows,
            });
            outBounds.Expand(worldBounds);
        }
    }
}
