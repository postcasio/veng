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
                      const Entity exclude)
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
            if (renderer == nullptr || !renderer->Mesh.IsLoaded())
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
            out.push_back(VisibleMesh{
                .Owner = dense[i],
                .World = world,
                .WorldBounds = worldBounds,
                .Mesh = renderer->Mesh.Get(),
                .CastsShadows = renderer->CastsShadows,
            });
            outBounds.Expand(worldBounds);
        }
    }
}
