#pragma once

#include <Veng/Veng.h>
#include <Veng/Math/AABB.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Entity.h>

namespace Veng
{
    class Scene;

    /// @brief Returns the local-to-parent matrix of a single Transform.
    ///
    /// Composed as T * R * S (scale first, rotate, then translate, applied to a
    /// column vector on the right).
    [[nodiscard]] mat4 LocalMatrix(const Transform& transform);

    /// @brief Returns the world matrix of an entity, composed up the Parent chain (root to entity).
    ///
    /// An entity with no Transform contributes identity at its level. A Parent cycle
    /// or a Parent referencing a dead entity is API misuse and a fatal VE_ASSERT.
    [[nodiscard]] mat4 WorldMatrix(const Scene& scene, Entity entity);

    /// @brief Fills out with the world matrix of every entity that has a Transform, in pool dense order.
    ///
    /// Recomputed on demand — no dirty-flag cache.
    void ComputeWorldMatrices(const Scene& scene, vector<mat4>& out);

    /// @brief Returns the world-space AABB bounding every resident (Transform, MeshRenderer) entity's mesh.
    ///
    /// Each mesh's local bound is transformed by the entity's world matrix and
    /// unioned. A non-resident mesh handle (not IsLoaded()) contributes nothing, so
    /// a still-loading scene bounds to what is loaded. Returns AABB::Empty() when
    /// no resident mesh renderers exist. Recomputed on demand, no cached bound;
    /// computes world matrices once via ComputeWorldMatrices.
    [[nodiscard]] AABB SceneBounds(const Scene& scene);
}
