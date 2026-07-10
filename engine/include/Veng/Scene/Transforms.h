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

    /// @brief Blends two Transforms component-wise by @p alpha.
    ///
    /// Position and scale are linearly interpolated; rotation is spherically interpolated (the
    /// shortest-arc slerp). @p alpha at 0 returns @p from, at 1 returns @p to. The render gather
    /// blends the last two Sim-tick snapshots by the frame's interpolation alpha through this, so a
    /// fixed-rate sim renders smoothly at any frame rate.
    /// @param from   The earlier (previous-tick) transform.
    /// @param to     The later (current-tick) transform.
    /// @param alpha  The interpolation fraction in [0, 1].
    /// @return The interpolated transform.
    [[nodiscard]] Transform InterpolateTransform(const Transform& from, const Transform& to,
                                                 f32 alpha);

    /// @brief Returns the world matrix of an entity, composed up the Hierarchy chain (root to entity).
    ///
    /// An entity with no Transform contributes identity at its level. A Hierarchy
    /// cycle or a parent link referencing a dead entity is API misuse and a fatal
    /// VE_ASSERT.
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
