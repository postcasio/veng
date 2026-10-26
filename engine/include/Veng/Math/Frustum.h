#pragma once

#include <array>

#include <Veng/Veng.h>
#include <Veng/Math/AABB.h>

namespace Veng
{
    /// @brief Six bounding half-spaces extracted from a view-projection matrix.
    ///
    /// glm-only value type (copied freely like AABB/mat4, no ownership rule),
    /// so it stays inside the public/backend include-hygiene split.
    struct VE_API Frustum
    {
        /// @brief The six clip planes — left, right, bottom, top, then depth 0 and depth 1.
        ///
        /// Under the engine's reverse-Z camera projection the depth-0 plane is the far plane and the
        /// depth-1 plane the near one; under a forward-Z projection they are the other way round.
        ///
        /// Each plane is vec4(nx, ny, nz, d) with an inward-pointing normal; a point p is
        /// inside the frustum when dot(plane.xyz, p) + plane.w >= 0 for all six planes.
        /// The normals are NOT normalized: Intersects is a sign-only test, so unit length
        /// is unnecessary.
        std::array<vec4, 6> Planes;

        /// @brief Extracts the six frustum planes from a view-projection matrix (Gribb-Hartmann, Vulkan ZO).
        ///
        /// Each plane is one clip-volume inequality read straight off the matrix rows.
        /// left/right/bottom/top are row4 ±/+ {row1,row2}; the Vulkan ZO depth-0 plane is the third
        /// clip row alone (clip.z >= 0) and the depth-1 plane is row4 - row3 (not the OpenGL
        /// row4 +/- row3 pair).
        /// The inward orientation is automatic by construction for any ZO projection, including the
        /// engine's Y-flipped one (the flip swaps which plane is geometrically "top" vs "bottom",
        /// which a cull testing all six never names). The planes land in the space the matrix maps
        /// from (world space for a view-projection), so a world-space AABB tests directly.
        [[nodiscard]] static Frustum FromViewProjection(const mat4& viewProj);
    };

    /// @brief Conservative AABB-vs-frustum intersection test.
    ///
    /// Returns false only when `box` lies wholly outside one plane (positive-vertex test:
    /// the box corner farthest along the plane's normal is still behind it). A box straddling
    /// a plane, or just outside a frustum corner, returns true — a false positive that draws,
    /// never a false negative that culls a visible box.
    [[nodiscard]] VE_API bool Intersects(const Frustum& frustum, const AABB& box);
}
