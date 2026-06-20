#pragma once

#include <array>
#include <limits>

#include <Veng/Veng.h>

// AABB — an axis-aligned bounding box: a min/max vec3 pair. The engine's first
// bounds primitive. glm-only value-type math (copied freely like vec3/mat4, no
// ownership rule), so it stays inside the public/backend include-hygiene split.

namespace Veng
{
    struct AABB
    {
        vec3 Min;
        vec3 Max;

        // The empty box: Min > Max on every axis, the identity for Union/Expand.
        // Folding over zero points or boxes yields Empty(), not a degenerate box
        // at the origin.
        [[nodiscard]] static AABB Empty()
        {
            constexpr f32 inf = std::numeric_limits<f32>::infinity();
            return AABB{vec3(inf), vec3(-inf)};
        }

        // True when any axis has Min > Max — the Empty() box and any box never
        // grown by a point or another box.
        [[nodiscard]] bool IsEmpty() const
        {
            return Min.x > Max.x || Min.y > Max.y || Min.z > Max.z;
        }

        [[nodiscard]] vec3 Center() const { return (Min + Max) * 0.5f; }
        [[nodiscard]] vec3 Extents() const { return (Max - Min) * 0.5f; }
        [[nodiscard]] vec3 Size() const { return Max - Min; }

        void Expand(vec3 point); // grow to include a point
        void Expand(const AABB& other); // grow to include another box

        // The 8 corners, for transform-and-refit and frustum tests.
        [[nodiscard]] std::array<vec3, 8> Corners() const;

        // A new axis-aligned box bounding this box's 8 corners under `m`. The
        // standard transform-and-refit: an AABB transformed by a rotation is the
        // AABB of the rotated corners, not the rotated min/max — so a rotation
        // grows the box rather than shearing it.
        [[nodiscard]] AABB Transformed(const mat4& m) const;
    };

    // Union as a free function so it reads at the call site; Empty() is its
    // identity (Union(Empty(), b) == b).
    [[nodiscard]] AABB Union(const AABB& a, const AABB& b);
}
