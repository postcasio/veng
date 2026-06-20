#pragma once

#include <array>

#include <Veng/Veng.h>
#include <Veng/Math/AABB.h>

// Frustum — six bounding half-spaces extracted from a view-projection matrix.
// The second bounds primitive beside AABB: glm-only value-type math (copied
// freely like AABB/mat4, no ownership rule), so it stays inside the
// public/backend include-hygiene split.

namespace Veng
{
    struct Frustum
    {
        // Six bounding half-spaces — left, right, bottom, top, near, far. Each is
        // a plane vec4(nx, ny, nz, d) with an inward-pointing normal, so a point p
        // is inside the frustum when dot(plane.xyz, p) + plane.w >= 0 for all six.
        // The normals are NOT normalized: Intersects is a sign-only test, so unit
        // length is unnecessary (normalizing for a true signed distance is a
        // one-line add if a distance-based consumer ever needs it).
        std::array<vec4, 6> Planes;

        // Gribb-Hartmann: each plane is one clip-volume inequality of the combined
        // view-projection matrix, read straight off its rows. left/right/bottom/top
        // are row4 -/+ {row1,row2}; the Vulkan ZO near plane is the third clip row
        // alone (clip.z >= 0) and far is row4 - row3 (NOT the OpenGL row4 +/- row3
        // pair). Because each plane IS the inequality the inside-frustum region
        // satisfies by construction, the inward orientation is automatic for any ZO
        // projection the matrix carries — including the engine's Y-flipped one (the
        // flip swaps which plane is geometrically "top" vs "bottom", which a cull
        // that tests all six never names). No sign-correction or per-plane
        // reorientation step exists; the one ZO-specific row is the near plane.
        //
        // The planes land in the space the matrix maps from (world, for a
        // view-projection), so a world-space AABB tests directly.
        [[nodiscard]] static Frustum FromViewProjection(const mat4& viewProj);
    };

    // Conservative AABB-vs-frustum test. Returns false only when `box` lies wholly
    // outside one plane (the standard positive-vertex test: the box corner farthest
    // along each plane's normal is still behind it). A box straddling a plane, or
    // just outside a frustum corner, returns true — a false positive that draws,
    // never a false negative that culls a visible box.
    [[nodiscard]] bool Intersects(const Frustum& frustum, const AABB& box);
}
