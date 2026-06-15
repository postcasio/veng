#pragma once

#include <Veng/Veng.h>
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
    // that pool's dense order. v1 recomputes on demand — there is no dirty-flag
    // cache. Equivalent to calling WorldMatrix per Transform-bearing entity.
    void ComputeWorldMatrices(const Scene& scene, vector<mat4>& out);
}
