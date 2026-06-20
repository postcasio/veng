#include <Veng/Scene/SceneBroadphase.h>

#include <algorithm>

#include <Veng/Asset/Mesh.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

namespace Veng
{
    void SceneBroadphase::Sync(const Scene& scene)
    {
        const u64 version = scene.GetSpatialVersion();

        bool needRebuild = (version != m_LastVersion);

        // A mesh finishing async load does not mutate the scene, so it does not bump
        // the spatial version. While candidates are still resolving residency, poll
        // only the tracked pending entities: a load completing grew the candidate set
        // without a version move, so rebuild. A dead pending entity is dropped.
        if (!needRebuild && !m_Pending.empty())
        {
            usize live = 0;
            for (const Entity entity : m_Pending)
            {
                if (!scene.IsAlive(entity))
                {
                    continue;
                }

                const auto* renderer = scene.TryGet<MeshRenderer>(entity);
                if (renderer != nullptr && renderer->Mesh.IsLoaded())
                {
                    needRebuild = true;
                }

                m_Pending[live++] = entity;
            }
            m_Pending.resize(live);
        }

        if (needRebuild)
        {
            Rebuild(scene);
            m_LastVersion = version;
            m_DidRebuild = true;
        }
        else
        {
            m_DidRebuild = false;
        }
    }

    void SceneBroadphase::Rebuild(const Scene& scene)
    {
        GatherMeshes(scene, m_Candidates, m_SceneBounds);

        // One BVH leaf per submesh: a frustum rejects an off-screen submesh of an on-screen
        // mesh by the same tree descent. The candidate list is in mesh-then-submesh order, so
        // a mesh's submeshes are contiguous (the g-buffer pass relies on this to skip redundant
        // vertex/index binds).
        m_SubMeshCandidates.clear();
        m_LeafScratch.clear();
        for (u32 meshIndex = 0; meshIndex < m_Candidates.size(); ++meshIndex)
        {
            const VisibleMesh& candidate = m_Candidates[meshIndex];
            const std::span<const SubMesh> subMeshes = candidate.Mesh->GetSubMeshes();
            for (u32 subMeshIndex = 0; subMeshIndex < subMeshes.size(); ++subMeshIndex)
            {
                const u32 id = static_cast<u32>(m_SubMeshCandidates.size());
                m_SubMeshCandidates.push_back(
                    SubMeshCandidate{.MeshCandidate = meshIndex, .SubMeshIndex = subMeshIndex});
                m_LeafScratch.push_back(BVH::Leaf{
                    .Box = subMeshes[subMeshIndex].Bounds.Transformed(candidate.World), .Id = id});
            }
        }

        m_Tree.Build(m_LeafScratch);

        // Record entities whose mesh is not yet resident so a later Sync rebuilds
        // when one loads; const View avoids bumping the spatial version.
        m_Pending.clear();
        for (auto [entity, transform, renderer] : scene.View<Transform, MeshRenderer>())
        {
            (void)transform;
            if (!renderer.Mesh.IsLoaded())
            {
                m_Pending.push_back(entity);
            }
        }
    }

    void SceneBroadphase::Cull(const Frustum& frustum, vector<u32>& out) const
    {
        const usize start = out.size();
        m_Tree.Query(frustum, out);
        // Sort the appended range ascending so draws issue in GatherMeshes order;
        // an earlier-appended range is left untouched.
        std::sort(out.begin() + static_cast<std::ptrdiff_t>(start), out.end());
    }
}
