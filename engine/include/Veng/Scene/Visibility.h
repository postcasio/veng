#pragma once

#include <Veng/Veng.h>
#include <Veng/Math/AABB.h>
#include <Veng/Scene/Entity.h>

namespace Veng
{
    class Scene;
    class Mesh;

    /// @brief One resident drawable candidate from a (Transform, MeshRenderer) entity.
    ///
    /// Carries the entity's world matrix, world-space bound, and resident mesh.
    /// Built per frame and valid for exactly the Execute that gathered it — Mesh
    /// points into the MeshRenderer's resident AssetHandle, and no garbage
    /// collection or handle mutation runs mid-Execute. A consumer must not reuse
    /// a span of these across frames.
    struct VisibleMesh
    {
        /// @brief The entity that owns this drawable.
        Entity Owner;
        /// @brief Entity's world matrix.
        mat4 World;
        /// @brief World-space AABB of the mesh.
        AABB WorldBounds;
        /// @brief Resident mesh pointer; valid for the gathering Execute only.
        const Mesh* Mesh;
    };

    /// @brief One per-submesh draw candidate: a gather record and the submesh within it.
    ///
    /// The broadphase's leaf granularity. MeshCandidate indexes the per-mesh VisibleMesh
    /// span (GatherMeshes order); SubMeshIndex selects the submesh in that mesh's
    /// GetSubMeshes(). A SceneBroadphase::Cull id indexes the flat candidate list these
    /// form, mapping a frustum survivor back to the mesh + submesh a draw needs.
    struct SubMeshCandidate
    {
        /// @brief Index into the per-mesh VisibleMesh span.
        u32 MeshCandidate;
        /// @brief Index into the mesh's GetSubMeshes() span.
        u32 SubMeshIndex;
    };

    /// @brief Gathers every resident (Transform, MeshRenderer) entity into out and unions their world bounds.
    ///
    /// Clears out first, then fills it in Transform pool dense order. Each entry's
    /// WorldBounds = Mesh->GetBounds().Transformed(world). A non-resident mesh
    /// handle (not IsLoaded()) is skipped. outBounds equals SceneBounds(scene) and
    /// is AABB::Empty() when no resident mesh renderers exist. No culling is
    /// applied — this is the unculled candidate set.
    /// @param scene     Scene whose resident (Transform, MeshRenderer) entities are gathered.
    /// @param out       Destination; cleared then filled with gathered candidates.
    /// @param outBounds Receives the union of all gathered world bounds.
    void GatherMeshes(const Scene& scene, vector<VisibleMesh>& out, AABB& outBounds);
}
