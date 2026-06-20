// Punctual shadow-view math unit cases: pure CPU math, no Context, no Vulkan.
// ComputeSpotShadowView / ComputePointShadowView are glm-only functions of a
// light's position/direction/range/cone, so these run with no ICD — the
// shadow_cascades.cpp / frustum.cpp pattern.

#include <doctest/doctest.h>

#include <array>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

#include <Veng/Renderer/PunctualShadows.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // The NDC projection of a world point through a view-proj: clip / w.
    vec3 ProjectNdc(const mat4& viewProj, const vec3& world)
    {
        const vec4 clip = viewProj * vec4(world, 1.0f);
        return vec3(clip) / clip.w;
    }

    bool IsFinite(const mat4& m)
    {
        for (int c = 0; c < 4; ++c)
            for (int r = 0; r < 4; ++r)
                if (!std::isfinite(m[c][r]))
                    return false;
        return true;
    }

    // The six canonical cube-face forward axes, in CubeFace order.
    constexpr std::array<vec3, CubeFaceCount> FaceAxes = {
        vec3(1.0f, 0.0f, 0.0f), vec3(-1.0f, 0.0f, 0.0f),
        vec3(0.0f, 1.0f, 0.0f), vec3(0.0f, -1.0f, 0.0f),
        vec3(0.0f, 0.0f, 1.0f), vec3(0.0f, 0.0f, -1.0f)};
}

TEST_CASE("ComputeSpotShadowView: axis point projects to the tile center, z in [0,1]")
{
    const vec3 position(2.0f, 3.0f, -1.0f);
    const vec3 direction(0.0f, 0.0f, -1.0f);
    const f32 range = 20.0f;
    const f32 outerCone = glm::radians(30.0f);

    const SpotShadowView view = ComputeSpotShadowView(position, direction, range, outerCone);

    // A point on the cone axis at half range projects to xy ≈ (0,0), z ∈ [0,1].
    const vec3 onAxis = position + glm::normalize(direction) * (range * 0.5f);
    const vec3 ndc = ProjectNdc(view.ViewProj, onAxis);
    CHECK(ndc.x == doctest::Approx(0.0f).epsilon(0.001));
    CHECK(ndc.y == doctest::Approx(0.0f).epsilon(0.001));
    CHECK(ndc.z >= -1e-3f);
    CHECK(ndc.z <= 1.0f + 1e-3f);
}

TEST_CASE("ComputeSpotShadowView: cone containment — edge near the boundary, outside off the tile")
{
    const vec3 position(0.0f, 0.0f, 0.0f);
    const vec3 direction(0.0f, 0.0f, -1.0f);
    const f32 range = 20.0f;
    const f32 outerCone = glm::radians(30.0f);

    const SpotShadowView view = ComputeSpotShadowView(position, direction, range, outerCone);

    const f32 depth = range * 0.5f;
    // A point at the outer-cone edge at half range: offset by tan(outerCone)*depth
    // along +X. The fovy = 2·outerCone frustum half-angle is outerCone, so this
    // sits exactly on the frustum's right edge → |x| ≈ 1.
    const f32 edgeOffset = std::tan(outerCone) * depth;
    const vec3 onEdge(edgeOffset, 0.0f, -depth);
    const vec3 edgeNdc = ProjectNdc(view.ViewProj, onEdge);
    CHECK(std::abs(edgeNdc.x) == doctest::Approx(1.0f).epsilon(0.01));
    CHECK(std::abs(edgeNdc.y) <= 1.0f + 1e-3f);

    // A point just outside the outer cone projects off the tile (|x| > 1), so
    // nothing the lighting pass lights samples outside the map.
    const vec3 outside(edgeOffset * 1.2f, 0.0f, -depth);
    const vec3 outNdc = ProjectNdc(view.ViewProj, outside);
    CHECK(std::abs(outNdc.x) > 1.0f);
}

TEST_CASE("ComputeSpotShadowView: range fit — far at range, near clips, carried Near/Far")
{
    const vec3 position(0.0f, 0.0f, 0.0f);
    const vec3 direction(0.0f, 0.0f, -1.0f);
    const f32 range = 20.0f;
    const f32 outerCone = glm::radians(25.0f);

    const SpotShadowView view = ComputeSpotShadowView(position, direction, range, outerCone);

    // Far == range, Near == the Range-relative fraction (0.05·range, floored).
    CHECK(view.Far == doctest::Approx(range));
    CHECK(view.Near == doctest::Approx(std::max(range * 0.05f, 0.05f)));

    // A point at range along the axis projects to z ≈ 1.
    const vec3 atFar = position + glm::normalize(direction) * range;
    CHECK(ProjectNdc(view.ViewProj, atFar).z == doctest::Approx(1.0f).epsilon(0.001));

    // A point nearer than Near projects to z < 0 (clipped by the near plane).
    const vec3 nearer = position + glm::normalize(direction) * (view.Near * 0.5f);
    CHECK(ProjectNdc(view.ViewProj, nearer).z < 0.0f);
}

TEST_CASE("ComputeSpotShadowView: straight-down and straight-up spots stay non-degenerate")
{
    const vec3 position(0.0f, 10.0f, 0.0f);
    const f32 range = 20.0f;
    const f32 outerCone = glm::radians(30.0f);

    const SpotShadowView down = ComputeSpotShadowView(position, vec3(0.0f, -1.0f, 0.0f), range, outerCone);
    const SpotShadowView up = ComputeSpotShadowView(position, vec3(0.0f, 1.0f, 0.0f), range, outerCone);

    CHECK(IsFinite(down.ViewProj));
    CHECK(IsFinite(up.ViewProj));

    // The stableUp swap holds: a point along the axis still projects to the tile
    // center with z ∈ [0,1].
    const vec3 belowNdc = ProjectNdc(down.ViewProj, position + vec3(0.0f, -range * 0.5f, 0.0f));
    CHECK(belowNdc.x == doctest::Approx(0.0f).epsilon(0.001));
    CHECK(belowNdc.y == doctest::Approx(0.0f).epsilon(0.001));
    CHECK(belowNdc.z >= -1e-3f);
    CHECK(belowNdc.z <= 1.0f + 1e-3f);
}

TEST_CASE("ComputePointShadowView: each axis hits its matching face and no other")
{
    const vec3 position(1.0f, -2.0f, 3.0f);
    const f32 range = 15.0f;

    const PointShadowView view = ComputePointShadowView(position, range);

    CHECK(view.Far == doctest::Approx(range));
    CHECK(view.Near == doctest::Approx(std::max(range * 0.05f, 0.05f)));

    for (u32 axis = 0; axis < CubeFaceCount; ++axis)
    {
        const vec3 point = position + FaceAxes[axis] * (range * 0.5f);

        // The matching face: the point projects to its tile center, z ∈ [0,1].
        const vec3 matchNdc = ProjectNdc(view.ViewProj[axis], point);
        CHECK(matchNdc.x == doctest::Approx(0.0f).epsilon(0.001));
        CHECK(matchNdc.y == doctest::Approx(0.0f).epsilon(0.001));
        CHECK(matchNdc.z >= -1e-3f);
        CHECK(matchNdc.z <= 1.0f + 1e-3f);

        // Every other face: the point is outside [-1,1]² or outside z ∈ [0,1] —
        // the six faces partition the sphere with the major-axis select's seams.
        for (u32 other = 0; other < CubeFaceCount; ++other)
        {
            if (other == axis)
                continue;
            const vec3 ndc = ProjectNdc(view.ViewProj[other], point);
            const bool outside = std::abs(ndc.x) > 1.0f + 1e-3f || std::abs(ndc.y) > 1.0f + 1e-3f ||
                                 ndc.z < -1e-3f || ndc.z > 1.0f + 1e-3f;
            CHECK(outside);
        }
    }
}

TEST_CASE("ComputePointShadowView: a seam direction reads consistent depth from both faces")
{
    const vec3 position(0.0f, 0.0f, 0.0f);
    const f32 range = 15.0f;

    const PointShadowView view = ComputePointShadowView(position, range);

    // A direction on the +X/+Y face boundary: it lies on the shared edge of both
    // faces (CubeFace 0 = +X, CubeFace 2 = +Y), so it projects near |xy| ≈ 1 of
    // each with a matching depth — the seam consistency Plan 04's cross-fade leans on.
    const vec3 point = glm::normalize(vec3(1.0f, 1.0f, 0.0f)) * (range * 0.5f);

    const vec3 ndcX = ProjectNdc(view.ViewProj[0], point);
    const vec3 ndcY = ProjectNdc(view.ViewProj[2], point);

    // On both faces the point is at the edge of the tile.
    CHECK(std::max(std::abs(ndcX.x), std::abs(ndcX.y)) == doctest::Approx(1.0f).epsilon(0.01));
    CHECK(std::max(std::abs(ndcY.x), std::abs(ndcY.y)) == doctest::Approx(1.0f).epsilon(0.01));

    // Both faces read the same NDC depth for the shared point (the perspective
    // depth depends only on the distance along each face's forward axis, which is
    // equal at a 45° seam).
    CHECK(ndcX.z == doctest::Approx(ndcY.z).epsilon(0.001));
}
