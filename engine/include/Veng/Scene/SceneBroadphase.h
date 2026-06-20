#pragma once

#include <span>

#include <Veng/Veng.h>
#include <Veng/Math/AABB.h>
#include <Veng/Math/BVH.h>
#include <Veng/Math/Frustum.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Scene/Visibility.h>

namespace Veng
{
    class Scene;

    // A consumer-owned spatial broadphase: it keeps a BVH current with a Scene by
    // rebuilding the tree on a spatial-version change (and while meshes are still
    // resolving residency). It holds the tree, the gathered candidate list, the
    // last-seen version, and the small set of candidates still loading, and exposes
    // a frustum query the scene passes call instead of a linear Intersects scan.
    //
    // It is NOT Scene state: one Scene is read by N renderers (editor previews),
    // each wanting its own tree lifetime. The Scene owns only the spatial-version
    // counter (the record that its own spatial state moved); each broadphase derives
    // its tree and candidate set from the scene on the frames the version moves. The
    // broadphase reads the scene const-only, so Sync never bumps the version.
    class SceneBroadphase
    {
    public:
        // Bring the tree current with `scene`. Rebuilds (re-gather + BVH::Build) iff
        // the scene's spatial version moved since the last sync, or a mesh that was
        // still loading has become resident. A cheap version compare otherwise.
        void Sync(const Scene& scene);

        // The live draw candidates, in GatherMeshes order; a Cull id indexes this.
        [[nodiscard]] std::span<const VisibleMesh> GetCandidates() const { return m_Candidates; }

        // Append the candidate indices visible to `frustum`, ascending (so draws
        // issue in GatherMeshes order, identical to the pre-BVH linear scan). `out`
        // is the caller's reused scratch, cleared by the caller; this appends.
        void Cull(const Frustum& frustum, vector<u32>& out) const;

        // The union of the live candidates' world bounds (for ComputeCascades) —
        // exactly the outBounds the gather produced, i.e. SceneBounds(scene).
        [[nodiscard]] AABB GetSceneBounds() const { return m_SceneBounds; }

        // Whether the most recent Sync rebuilt the tree (false on a fully static
        // frame). Diagnostics; the rendered image is identical regardless.
        [[nodiscard]] bool DidRebuildLastSync() const { return m_DidRebuild; }
        [[nodiscard]] u32 GetNodeCount() const { return m_Tree.GetNodeCount(); }

    private:
        // Re-gather the candidates, rebuild the tree over them, and refresh the
        // pending (not-yet-resident) set.
        void Rebuild(const Scene& scene);

        BVH m_Tree;
        vector<VisibleMesh> m_Candidates;  // dense, GatherMeshes order; Cull id == index
        vector<BVH::Leaf> m_LeafScratch;   // reused: tight box + candidate index per leaf
        vector<Entity> m_Pending;          // (Transform, MeshRenderer) entities not yet IsLoaded
        AABB m_SceneBounds = AABB::Empty();
        u64 m_LastVersion = ~0ull;         // != any real version → first Sync rebuilds
        bool m_DidRebuild = false;
    };
}
