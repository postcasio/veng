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

    /// @brief Consumer-owned spatial broadphase that keeps a BVH current with a Scene.
    ///
    /// Rebuilds the tree when the scene's spatial version changes or a still-loading
    /// mesh becomes resident; otherwise a Sync is a cheap version compare. Holds the
    /// BVH, the gathered candidate list, the last-seen version, and the set of
    /// not-yet-resident candidates.
    ///
    /// SceneBroadphase is not Scene state: one Scene may be read by multiple
    /// renderers, each with its own broadphase and tree lifetime. The Scene owns only
    /// the spatial-version counter; each broadphase derives its tree from the scene
    /// on the frames the version moves. Sync reads the scene const-only and never
    /// bumps the spatial version.
    class SceneBroadphase
    {
    public:
        /// @brief Brings the tree current with scene, omitting one nominated entity.
        ///
        /// Rebuilds (re-gather + BVH::Build) iff the scene's spatial version moved
        /// since the last Sync, a mesh that was still loading has become resident, or
        /// @p exclude differs from the entity the current tree was gathered against.
        ///
        /// The excluded entity is dropped by the gather itself, so it is absent from
        /// every consumer of this broadphase — the candidate list, the per-submesh
        /// leaves, both bounds, and therefore every draw and depth write derived from
        /// them. There is no domain or pass in which it survives.
        /// @param scene    Scene to bring the tree current with.
        /// @param exclude  One entity to omit from the tree entirely; Entity::Null omits none.
        void Sync(const Scene& scene, Entity exclude = Entity::Null);

        /// @brief Returns the live per-mesh gather records in GatherMeshes order.
        ///
        /// The scene-bound and shadow-view consumers read these (world matrix + world
        /// bound + resident mesh). A Cull id does NOT index this span — it indexes the
        /// per-submesh candidate list (GetSubMeshCandidates); a candidate's MeshCandidate
        /// field indexes here.
        [[nodiscard]] std::span<const VisibleMesh> GetCandidates() const { return m_Candidates; }

        /// @brief Returns the flat per-submesh draw candidates a Cull id indexes.
        ///
        /// One entry per submesh of each gathered mesh, in GatherMeshes order then submesh
        /// order — so a mesh's submesh candidates are contiguous. The broadphase's leaf
        /// granularity; a Cull survivor id indexes this span.
        [[nodiscard]] std::span<const SubMeshCandidate> GetSubMeshCandidates() const
        {
            return m_SubMeshCandidates;
        }

        /// @brief Appends per-submesh candidate ids visible to frustum, in ascending order.
        ///
        /// Each id indexes GetSubMeshCandidates(). `out` is the caller's reused scratch,
        /// cleared by the caller; this appends.
        void Cull(const Frustum& frustum, vector<u32>& out) const;

        /// @brief Returns the union of all live candidates' world bounds.
        ///
        /// Equals SceneBounds(scene) as of the last Sync; used by ComputeCascades.
        [[nodiscard]] AABB GetSceneBounds() const { return m_SceneBounds; }

        /// @brief Returns the union of the shadow-casting candidates' world bounds.
        ///
        /// The box the shadow projections fit to — every candidate whose MeshRenderer::CastsShadows
        /// is set. Equals GetSceneBounds() when every candidate casts, and AABB::Empty() when none
        /// do (or the scene is empty).
        [[nodiscard]] AABB GetCasterBounds() const { return m_CasterBounds; }

        /// @brief Returns true if the most recent Sync rebuilt the tree.
        ///
        /// False on a fully static frame. The rendered image is identical either way.
        [[nodiscard]] bool DidRebuildLastSync() const { return m_DidRebuild; }
        /// @brief Returns the number of nodes in the BVH.
        [[nodiscard]] u32 GetNodeCount() const { return m_Tree.GetNodeCount(); }

    private:
        /// @brief Re-gathers candidates, rebuilds the BVH, and refreshes the pending set.
        /// @param scene    Scene to gather from.
        /// @param exclude  One entity the gather omits; Entity::Null omits none.
        void Rebuild(const Scene& scene, Entity exclude);

        /// @brief The bounding-volume hierarchy over the gathered candidates.
        BVH m_Tree;
        /// @brief Dense per-mesh gather records in GatherMeshes order.
        vector<VisibleMesh> m_Candidates;
        /// @brief Flat per-submesh candidates; one BVH leaf each, a Cull id is an index.
        vector<SubMeshCandidate> m_SubMeshCandidates;
        /// @brief Reused per rebuild: tight box + candidate index per leaf.
        vector<BVH::Leaf> m_LeafScratch;
        /// @brief (Transform, MeshRenderer) entities whose mesh is not yet resident.
        vector<Entity> m_Pending;
        /// @brief World-space union of all gathered candidate bounds.
        AABB m_SceneBounds = AABB::Empty();
        /// @brief World-space union of the shadow-casting candidates' bounds.
        AABB m_CasterBounds = AABB::Empty();
        /// @brief != any real version on construction, so the first Sync rebuilds.
        u64 m_LastVersion = ~0ull;
        /// @brief The entity the current tree was gathered without; a change forces a rebuild.
        ///
        /// The scene's spatial version does not move when a caller changes which entity it
        /// excludes, so the exclusion is its own rebuild trigger.
        Entity m_LastExclude = Entity::Null;
        /// @brief Set by the most recent Sync call.
        bool m_DidRebuild = false;
    };
}
