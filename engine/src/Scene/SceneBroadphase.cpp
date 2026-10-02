#include <Veng/Scene/SceneBroadphase.h>

#include <algorithm>
#include <bit>

#include <Veng/Asset/Mesh.h>
#include <Veng/Diagnostics/Profiler.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

namespace Veng
{
    void SceneBroadphase::Reset()
    {
        m_Tree.Build({});
        m_Candidates.clear();
        m_GatherScratch.clear();
        m_SubMeshCandidates.clear();
        m_LeafBoxes.clear();
        m_Pending.clear();
        m_SceneBounds = AABB::Empty();
        m_CasterBounds = AABB::Empty();
        m_LastScene = nullptr;
        m_LastVersion = ~0ull;
        m_LastExclude = Entity::Null;
        m_LastLayerMask = AllRenderLayers;
    }

    void SceneBroadphase::Sync(const Scene& scene, const Entity exclude, const u32 layerMask)
    {
        VE_PROFILE_SCOPE("Render/BroadphaseSync");
        const u64 version = scene.GetSpatialVersion();

        // The spatial version is a per-scene counter, so a broadphase re-pointed at a different
        // scene (a persistent renderer whose presented world was swapped for another) can be handed
        // a version that coincides with the one it cached against the previous scene — and would then
        // keep serving the previous scene's candidates, whose Mesh/material/transform pointers name
        // the wrong scene's entities. Scene identity is therefore its own rebuild trigger, like the
        // view properties below: neither moves a spatial version.
        //
        // The exclusion and the layer mask are properties of the caller's view, not of the scene, so
        // neither moves a spatial version — each has to force its own rebuild or the tree keeps the
        // previous caller's candidate set.
        const bool viewChanged =
            (&scene != m_LastScene) || (exclude != m_LastExclude) || (layerMask != m_LastLayerMask);
        bool needGather = viewChanged || (version != m_LastVersion);

        // A mesh finishing async load does not mutate the scene, so it does not bump
        // the spatial version. While candidates are still resolving residency, poll
        // only the tracked pending entities: a load completing grew the candidate set
        // without a version move, so re-gather. A dead pending entity is dropped.
        if (!needGather && !m_Pending.empty())
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
                    needGather = true;
                }

                m_Pending[live++] = entity;
            }
            m_Pending.resize(live);
        }

        m_DidRebuild = false;
        m_DidRefit = false;
        if (needGather)
        {
            Update(scene, exclude, layerMask, !viewChanged);
            m_LastScene = &scene;
            m_LastVersion = version;
            m_LastExclude = exclude;
            m_LastLayerMask = layerMask;
        }
    }

    void SceneBroadphase::Update(const Scene& scene, const Entity exclude, const u32 layerMask,
                                 const bool allowRefit)
    {
        {
            VE_PROFILE_SCOPE("Render/GatherMeshes");
            GatherMeshes(scene, m_GatherScratch, m_SceneBounds, exclude, layerMask);
        }

        // The tree's topology is only worth keeping for the same leaves: the same entities drawing
        // the same meshes in the same order map every leaf id to the same submesh, so only the
        // boxes moved. Anything else (an entity added, removed or re-meshed, a reorder) rebuilds.
        const bool sameCandidates =
            allowRefit && m_Tree.GetLeafCount() > 0 &&
            std::ranges::equal(m_GatherScratch, m_Candidates,
                               [](const VisibleMesh& a, const VisibleMesh& b)
                               { return a.Owner == b.Owner && a.Mesh == b.Mesh; });
        std::swap(m_Candidates, m_GatherScratch);

        // The caster bound is the union of the shadow-casting candidates alone — the box the
        // shadow projections fit to, so a non-caster (an emissive body co-located with its own
        // light) never widens a shadow frustum. It equals m_SceneBounds when every candidate casts.
        m_CasterBounds = AABB::Empty();

        // One BVH leaf per submesh: a frustum rejects an off-screen submesh of an on-screen
        // mesh by the same tree descent. The candidate list is in mesh-then-submesh order, so
        // a mesh's submeshes are contiguous (the g-buffer pass relies on this to skip redundant
        // vertex/index binds). On a refit the candidate list already holds exactly these.
        if (!sameCandidates)
        {
            m_SubMeshCandidates.clear();
        }
        m_LeafBoxes.clear();
        for (u32 meshIndex = 0; meshIndex < m_Candidates.size(); ++meshIndex)
        {
            const VisibleMesh& candidate = m_Candidates[meshIndex];
            if (candidate.CastsShadows)
            {
                m_CasterBounds.Expand(candidate.WorldBounds);
            }
            const std::span<const SubMesh> subMeshes = candidate.Mesh->GetSubMeshes();
            for (u32 subMeshIndex = 0; subMeshIndex < subMeshes.size(); ++subMeshIndex)
            {
                if (!sameCandidates)
                {
                    m_SubMeshCandidates.push_back(
                        SubMeshCandidate{.MeshCandidate = meshIndex, .SubMeshIndex = subMeshIndex});
                }
                m_LeafBoxes.push_back(subMeshes[subMeshIndex].Bounds.Transformed(candidate.World));
            }
        }

        if (sameCandidates)
        {
            {
                VE_PROFILE_SCOPE("Render/BvhRefit");
                m_Tree.Refit(m_LeafBoxes);
            }
            m_DidRefit = m_Tree.GetCost() <= m_Tree.GetBuildCost() * RefitCostLimit;
        }
        if (!m_DidRefit)
        {
            RebuildTree();
            m_DidRebuild = true;
        }

        // Record entities whose mesh is not yet resident so a later Sync re-gathers
        // when one loads; const View avoids bumping the spatial version.
        m_Pending.clear();
        for (auto [entity, renderer] : scene.View<MeshRenderer>())
        {
            if (entity == exclude || !RenderLayerInMask(layerMask, renderer.Layer))
            {
                continue;
            }
            if (!renderer.Mesh.IsLoaded() && scene.Has<Transform>(entity))
            {
                m_Pending.push_back(entity);
            }
        }
    }

    void SceneBroadphase::RebuildTree()
    {
        VE_PROFILE_SCOPE("Render/BvhBuild");
        m_LeafScratch.resize(m_LeafBoxes.size());
        for (u32 id = 0; id < m_LeafBoxes.size(); ++id)
        {
            m_LeafScratch[id] = BVH::Leaf{.Box = m_LeafBoxes[id], .Id = id};
        }
        m_Tree.Build(m_LeafScratch);
    }

    void SceneBroadphase::Cull(const Frustum& frustum, vector<u32>& out) const
    {
        // Mark the survivors, then walk the words in order so they append ascending (GatherMeshes
        // order) without a sort; an earlier-appended range of `out` is left untouched.
        const usize words = (m_SubMeshCandidates.size() + 63) / 64;
        m_CullBits.assign(words, 0);
        m_Tree.QueryBits(frustum, m_CullBits);
        for (usize w = 0; w < words; ++w)
        {
            u64 bits = m_CullBits[w];
            while (bits != 0)
            {
                out.push_back(static_cast<u32>(w * 64) + static_cast<u32>(std::countr_zero(bits)));
                bits &= bits - 1;
            }
        }
    }
}
