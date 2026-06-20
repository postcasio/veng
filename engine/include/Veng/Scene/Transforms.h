#pragma once

#include <Veng/Veng.h>
#include <Veng/Math/AABB.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Entity.h>

namespace Veng
{
    class Scene;

    // The local-to-parent matrix of a single Transform: T * R * S (scale first,
    // then rotate, then translate, applied to a column vector on the right).
    [[nodiscard]] mat4 LocalMatrix(const Transform& transform);

    // The world matrix of an entity: parent.world * local composed up the Parent
    // chain (root → entity). An entity with no Transform contributes identity at
    // its level. A Parent cycle, or a Parent referencing a dead entity, is API
    // misuse and a fatal VE_ASSERT.
    [[nodiscard]] mat4 WorldMatrix(const Scene& scene, Entity entity);

    // Fills `out` with the world matrix of every entity that has a Transform, in
    // that pool's dense order. Recomputes on demand — there is no dirty-flag
    // cache. Equivalent to calling WorldMatrix per Transform-bearing entity.
    void ComputeWorldMatrices(const Scene& scene, vector<mat4>& out);

    // The world-space AABB bounding every resident (Transform, MeshRenderer)
    // entity's mesh — each mesh's local bound transformed by the entity's world
    // matrix and unioned. A non-resident mesh handle (not IsLoaded()) contributes
    // nothing, so a still-loading scene bounds to what is loaded. Returns
    // AABB::Empty() when the scene has no resident mesh renderers. Recomputed on
    // demand, no cached bound (mirrors ComputeWorldMatrices) — it computes world
    // matrices once via ComputeWorldMatrices rather than re-walking the parent
    // chain per entity.
    [[nodiscard]] AABB SceneBounds(const Scene& scene);
}
