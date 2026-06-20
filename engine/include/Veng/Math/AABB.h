#pragma once

#include <array>
#include <limits>

#include <Veng/Veng.h>

namespace Veng
{
    /// @brief Axis-aligned bounding box: a min/max vec3 pair.
    ///
    /// glm-only value type (copied freely like vec3/mat4, no ownership rule),
    /// so it stays inside the public/backend include-hygiene split.
    struct AABB
    {
        /// @brief Minimum corner.
        vec3 Min;
        /// @brief Maximum corner.
        vec3 Max;

        /// @brief Returns the empty box (Min > Max on every axis), the identity for Union/Expand.
        ///
        /// Folding over zero points or boxes yields Empty(), not a degenerate box at the origin.
        [[nodiscard]] static AABB Empty()
        {
            constexpr f32 inf = std::numeric_limits<f32>::infinity();
            return AABB{vec3(inf), vec3(-inf)};
        }

        /// @brief Returns true when any axis has Min > Max — the Empty() box and any box never grown.
        [[nodiscard]] bool IsEmpty() const
        {
            return Min.x > Max.x || Min.y > Max.y || Min.z > Max.z;
        }

        /// @brief Returns the center of the box.
        [[nodiscard]] vec3 Center() const { return (Min + Max) * 0.5f; }

        /// @brief Returns the half-extents of the box.
        [[nodiscard]] vec3 Extents() const { return (Max - Min) * 0.5f; }

        /// @brief Returns the full size of the box.
        [[nodiscard]] vec3 Size() const { return Max - Min; }

        /// @brief Grows the box to include a point.
        void Expand(vec3 point);

        /// @brief Grows the box to include another box.
        void Expand(const AABB& other);

        /// @brief Returns the 8 corners, for transform-and-refit and frustum tests.
        [[nodiscard]] std::array<vec3, 8> Corners() const;

        /// @brief Returns a new AABB bounding this box's 8 corners transformed by `m`.
        ///
        /// Standard transform-and-refit: an AABB transformed by a rotation is the AABB of the
        /// rotated corners, not the rotated min/max, so a rotation grows the box rather than shearing it.
        [[nodiscard]] AABB Transformed(const mat4& m) const;
    };

    /// @brief Returns the union of two boxes, i.e. the smallest AABB containing both.
    ///
    /// Empty() is the identity: Union(Empty(), b) == b.
    [[nodiscard]] AABB Union(const AABB& a, const AABB& b);
}
