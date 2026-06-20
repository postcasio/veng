#pragma once

#include <Veng/Veng.h>
#include <Veng/Math/AABB.h>
#include <Veng/Scene/Entity.h>

namespace Veng
{
    class Scene;
    class Mesh;

    // One resident drawable candidate (pre-cull — the name reflects "a mesh that
    // may be visible," not a post-cull result): a (Transform, MeshRenderer)
    // entity's world matrix, its world-space bound, and its resident mesh. Built
    // per frame and borrowed for the frame — Mesh points into the MeshRenderer's
    // resident AssetHandle, valid for exactly the Execute that gathered it because
    // no garbage collection or handle mutation runs mid-Execute. A cross-frame
    // consumer must not reuse the span across frames.
    struct VisibleMesh
    {
        Entity      Owner;
        mat4        World;
        AABB        WorldBounds;
        const Mesh* Mesh;
    };

    // Gather every resident (Transform, MeshRenderer) entity into `out` (cleared
    // first) and union their world bounds into `outBounds` (AABB::Empty() when
    // none). One pass over the Transform pool's dense order computes each world
    // matrix; each entry's WorldBounds = Mesh->GetBounds().Transformed(world). A
    // non-resident mesh handle (not IsLoaded()) is skipped, so an async-loading
    // scene gathers what is loaded. There is no culling — this is the unculled
    // candidate set; a consumer applies Intersects per frustum. outBounds is
    // exactly SceneBounds(scene), a free by-product of the same pass.
    // Recompute-on-demand, no cached visibility.
    void GatherMeshes(const Scene& scene, vector<VisibleMesh>& out, AABB& outBounds);
}
