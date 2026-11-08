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
        /// A projection with no far plane (Veng::InfiniteFar) gives a depth-0 plane with a zero
        /// normal and a positive w, which every point satisfies, so nothing is culled for distance.
        /// A consumer normalizing the planes must skip a zero-length one, as the sphere Intersects
        /// does.
        ///
        /// Each plane is vec4(nx, ny, nz, d) with an inward-pointing normal; a point p is
        /// inside the frustum when dot(plane.xyz, p) + plane.w >= 0 for all six planes.
        /// The normals are NOT normalized: Intersects is a sign-only test, so unit length
        /// is unnecessary.
        std::array<vec4, 6> Planes;

        /// @brief Extracts the six frustum planes from a view-projection matrix (Gribb-Hartmann, zero-to-one depth).
        ///
        /// Each plane is one clip-volume inequality read straight off the matrix rows.
        /// left/right/bottom/top are row4 ±/+ {row1,row2}; the zero-to-one depth-0 plane is the third
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

    /// @brief Conservative sphere-vs-frustum intersection test.
    ///
    /// Returns false only when the sphere lies wholly behind one plane. Near a frustum edge a
    /// sphere outside two planes at once still passes: a false positive, never a false negative.
    /// A plane with a zero normal (an infinite projection's far plane) is skipped.
    /// @param frustum  The frustum to test against.
    /// @param center   The sphere's centre, in the frustum's space.
    /// @param radius   The sphere's radius.
    /// @return Whether the sphere may reach inside the frustum.
    [[nodiscard]] VE_API bool Intersects(const Frustum& frustum, const vec3& center, f32 radius);
}
