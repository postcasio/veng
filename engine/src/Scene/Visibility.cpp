#include <Veng/Scene/Visibility.h>

#include <glm/gtc/matrix_inverse.hpp>

#include <Veng/Asset/Mesh.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/RemoteInterpolationSystem.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Transforms.h>

namespace Veng
{
    void GatherMeshes(const Scene& scene, vector<VisibleMesh>& out, AABB& outBounds,
                      const Entity exclude, const u32 layerMask)
    {
        scene.UpdateWorldTransforms();

        // Only a predicted entity being smoothed carries a residual, so the per-entity lookup is
        // skipped outright when none does.
        const bool anyPredictionError = scene.PoolCount(TypeIdOf<PredictionError>()) > 0;

        out.clear();
        outBounds = AABB::Empty();
        for (auto [entity, rendererRef] : scene.View<MeshRenderer>())
        {
            const MeshRenderer* renderer = &rendererRef;
            if (entity == exclude || !renderer->Visible || !renderer->Mesh.IsLoaded())
            {
                continue;
            }

            // The view's layer mask filters here, beside Visible, so nothing downstream re-tests it:
            // an off-mask renderer is absent from the candidate list and never widens outBounds.
            if (!RenderLayerInMask(layerMask, renderer->Layer) || !scene.Has<Transform>(entity))
            {
                continue;
            }

            // A predicted entity being visually smoothed after a reconciliation correction carries a
            // decaying render offset (position + rotation about its origin), applied only here — the
            // sim Transform stays authoritative; the render pose eases into it.
            mat4 world = WorldMatrix(scene, entity);
            if (anyPredictionError)
            {
                if (const auto* error = scene.TryGet<PredictionError>(entity))
                {
                    world = ApplyPredictionError(world, *error);
                }
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
                .Owner = entity,
                .World = world,
                .NormalMatrix = glm::inverseTranspose(mat3(world)),
                .WorldBounds = worldBounds,
                .Mesh = renderer->Mesh.Get(),
                .Materials = materials,
                .CastsShadows = renderer->CastsShadows,
                .SortPriority = renderer->SortPriority,
            });
            outBounds.Expand(worldBounds);
        }
    }
}
