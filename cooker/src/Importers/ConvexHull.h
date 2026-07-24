#pragma once

#include <Veng/Veng.h>

#include <span>

namespace Veng::Cook
{
    /// @brief Upper bound on a cooked hull's vertex count.
    ///
    /// A convex hull is a collision primitive, not a render mesh: past a few hundred vertices the
    /// solver pays for detail no contact resolution uses. The bound is generous enough that no
    /// reasonable source trips it and low enough that an accidental hull over a whole scene is a
    /// loud cook error rather than a silently expensive asset.
    inline constexpr usize MaxConvexHullPoints = 2048;

    /// @brief Reduces a point cloud to the vertices of its convex hull.
    ///
    /// A self-contained incremental hull: the cook owns its own implementation so the offline
    /// toolchain links no solver and the cooked blob names no third-party format. Duplicate and
    /// near-duplicate points are welded first, so a model exported with split vertices (per-face
    /// normals, UV seams) does not inflate the result.
    ///
    /// **Degenerate input is returned welded rather than rejected.** A point set that is empty,
    /// collinear, or coplanar has no three-dimensional hull; the welded points are still a valid
    /// description of the same shape, and a consumer building a solver shape from them reaches
    /// the same answer.
    /// @param points  The source point cloud.
    /// @param out     Destination, cleared then filled with the hull's vertices.
    /// @return False when the hull would exceed MaxConvexHullPoints; @p out is then unspecified.
    [[nodiscard]] bool BuildConvexHull(std::span<const vec3> points, vector<vec3>& out);

    /// @brief Welds coincident points and rewrites the triangle indices onto the welded set.
    ///
    /// A render mesh splits a vertex per normal and per UV seam, which a solver has no use for:
    /// welding by position alone gives the triangle mesh the connectivity the source surface
    /// actually has. Degenerate triangles (two or three indices welded together) are dropped.
    /// @param points   The source positions.
    /// @param indices  The source triangle indices into @p points.
    /// @param outPoints   Destination for the welded positions, cleared then filled.
    /// @param outIndices  Destination for the rewritten indices, cleared then filled.
    void WeldTriangleMesh(std::span<const vec3> points, std::span<const u32> indices,
                          vector<vec3>& outPoints, vector<u32>& outIndices);
}
