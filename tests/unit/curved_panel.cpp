// Primitives::CurvedPanel and the two closed-form companions that make one usable, on the CPU.
//
// The load-bearing case is that CurvedPanelHit inverts the generator: if the two disagree about the
// panel's parameterization a consumer's world-anchored marker drifts off its target and nothing else
// catches it, so every generated vertex is reprojected from an eye through the hit function and
// required to come back with that vertex's UV — at an eye inside the cylinder and at one outside it,
// where every hit is the far root.
//
// The frame and the UV orientation are checked with ProjectToScreen — the projection the renderer
// actually uses — rather than through the generator's own inverse, because a vertically mirrored
// document passes an inverse-based check: the mirror cancels.
//
// Beside them the sizing solve, closed round trip: size a panel for a screen rect, generate it,
// project its edge vertices, and require them on that rect's edges across radii spanning R < d,
// R = d and R >> d, plus the tight-curvature clamp and the flat limit against Plane's geometry.

#include <doctest/doctest.h>

#include <array>
#include <cmath>

#include <glm/geometric.hpp>
#include <glm/gtc/constants.hpp>

#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Primitives.h>
#include <Veng/Scene/Camera.h>

using namespace Veng;

namespace
{
    constexpr vec2 Extent{1920.0f, 1080.0f};

    // The pose a panel is designed to be viewed from: an eye on the panel's +Z normal at `distance`,
    // looking back down -Z with +Y up.
    CameraView PanelView(const f32 fovY, const f32 aspect, const f32 distance)
    {
        CameraView view;
        view.SetPerspective(fovY, aspect, 0.01f, 1000.0f);
        view.SetView(vec3(0.0f, 0.0f, distance), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));
        return view;
    }

    // A point on the panel's surface at a given UV, from the panel's stated frame rather than from
    // the generated mesh. Used only to aim rays at chosen places — never to predict a UV.
    vec3 SurfacePoint(const vec2 size, const f32 curvatureRadius, const vec2 uv)
    {
        const f32 angle = (uv.x * 2.0f - 1.0f) * size.x * 0.5f / curvatureRadius;
        return vec3(curvatureRadius * std::sin(angle), size.y * (0.5f - uv.y),
                    curvatureRadius * (1.0f - std::cos(angle)));
    }

    // The centre of curvature at a vertex's height: the axis point the surface is one radius from.
    vec3 AxisPoint(const f32 curvatureRadius, const vec3 position)
    {
        return vec3(0.0f, position.y, curvatureRadius);
    }
}

TEST_CASE("CurvedPanel is a cylinder section in its stated frame, spaced uniformly in arc length")
{
    struct Case
    {
        vec2 Size;
        f32 CurvatureRadius;
        uvec2 Subdivisions;
    };

    // The matrix spans the shipped shape, a single cell, the clamped-to-one degenerate grid, a wide
    // wrap, and a near-flat radius.
    const std::array<Case, 5> cases{
        Case{.Size = vec2(1.7f, 0.96f), .CurvatureRadius = 1.0f, .Subdivisions = uvec2(32, 8)},
        Case{.Size = vec2(1.7f, 0.96f), .CurvatureRadius = 0.75f, .Subdivisions = uvec2(1)},
        Case{.Size = vec2(2.0f, 1.0f), .CurvatureRadius = 0.5f, .Subdivisions = uvec2(0)},
        Case{.Size = vec2(3.0f, 1.0f), .CurvatureRadius = 0.5f, .Subdivisions = uvec2(9, 4)},
        Case{.Size = vec2(1.7f, 0.96f), .CurvatureRadius = 1000.0f, .Subdivisions = uvec2(16, 3)},
    };

    for (const Case& test : cases)
    {
        CAPTURE(test.Size.x);
        CAPTURE(test.CurvatureRadius);
        CAPTURE(test.Subdivisions.x);
        CAPTURE(test.Subdivisions.y);

        const MeshData data =
            Primitives::CurvedPanel(test.Size, test.CurvatureRadius, test.Subdivisions);

        // Zero subdivisions clamp to one cell per axis, exactly as Plane clamps.
        const uvec2 grid = glm::max(test.Subdivisions, uvec2(1));
        REQUIRE(data.Vertices.size() == static_cast<usize>(grid.x + 1) * (grid.y + 1));
        CHECK(data.Indices.size() == static_cast<usize>(grid.x) * grid.y * 6);

        const f32 halfAngle = test.Size.x * 0.5f / test.CurvatureRadius;

        for (const CanonicalVertex& vertex : data.Vertices)
        {
            // Every point is one radius from the axis through (0, y, +curvatureRadius) — the panel is
            // a section of that cylinder's lateral surface and flat along +Y.
            const vec3 axis = AxisPoint(test.CurvatureRadius, vertex.Position);
            CHECK(glm::length(vertex.Position - axis) ==
                  doctest::Approx(test.CurvatureRadius).epsilon(1e-5f));

            // The normal points at the axis, so the surface faces +Z and its flanks bend toward a
            // viewer out there; the tangent is unit and lies in the surface.
            CHECK(glm::length(vertex.Normal) == doctest::Approx(1.0f).epsilon(1e-5f));
            CHECK(glm::dot(vertex.Normal, axis - vertex.Position) > 0.0f);
            const vec3 tangent(vertex.Tangent);
            CHECK(glm::length(tangent) == doctest::Approx(1.0f).epsilon(1e-5f));
            CHECK(std::abs(glm::dot(tangent, vertex.Normal)) ==
                  doctest::Approx(0.0f).epsilon(1e-3f));

            // Straight along +Y: the height is a pure function of v, and v runs downward.
            CHECK(vertex.Position.y ==
                  doctest::Approx(test.Size.y * (0.5f - vertex.UV.y)).epsilon(1e-5f));

            // Uniform in arc length: the angle about the axis is linear in u, which is what makes the
            // columns evenly spaced on the glass rather than in the angle they subtend at any eye.
            const f32 angle =
                std::atan2(vertex.Position.x, test.CurvatureRadius - vertex.Position.z);
            CHECK(angle == doctest::Approx((vertex.UV.x * 2.0f - 1.0f) * halfAngle).epsilon(1e-4f));
        }

        // Consecutive columns are one cell of arc length apart, and the whole row spans size.x.
        const f32 cellArc = test.Size.x / static_cast<f32>(grid.x);
        for (u32 i = 0; i < grid.x; ++i)
        {
            const CanonicalVertex& from = data.Vertices[i];
            const CanonicalVertex& to = data.Vertices[i + 1];
            const f32 step = (std::atan2(to.Position.x, test.CurvatureRadius - to.Position.z) -
                              std::atan2(from.Position.x, test.CurvatureRadius - from.Position.z)) *
                             test.CurvatureRadius;
            CHECK(step == doctest::Approx(cellArc).epsilon(1e-4f));
        }

        // UVs are top-left origin and y-down: the first vertex is the panel's top-left corner as seen
        // from +Z (so above and to the -X side) and the last is its bottom-right.
        CHECK(data.Vertices.front().UV == vec2(0.0f, 0.0f));
        CHECK(data.Vertices.back().UV == vec2(1.0f, 1.0f));
        CHECK(data.Vertices.front().Position.y > data.Vertices.back().Position.y);
        CHECK(data.Vertices.front().Position.x < data.Vertices.back().Position.x);

        // Winding faces +Z: the geometric face normal agrees with the shading normals, so the
        // CCW-front surface pipeline does not cull the panel away from the viewer it faces. A facet's
        // own normal is the surface normal at its mid-angle, so it agrees with its corners' normals
        // only while a cell spans less than half a turn; past that the chord plane has passed the axis
        // and the comparison says nothing about the winding.
        if (2.0f * halfAngle / static_cast<f32>(grid.x) < glm::pi<f32>())
        {
            for (usize i = 0; i + 2 < data.Indices.size(); i += 3)
            {
                const CanonicalVertex& v0 = data.Vertices[data.Indices[i + 0]];
                const CanonicalVertex& v1 = data.Vertices[data.Indices[i + 1]];
                const CanonicalVertex& v2 = data.Vertices[data.Indices[i + 2]];
                const vec3 geometric =
                    glm::cross(v1.Position - v0.Position, v2.Position - v0.Position);
                CHECK(glm::dot(geometric, v0.Normal + v1.Normal + v2.Normal) > 0.0f);
            }
        }

        // One submesh, no material by default — the shared generator contract.
        REQUIRE(data.SubMeshes.size() == 1);
        CHECK(data.SubMeshes[0].IndexCount == data.Indices.size());
        CHECK(data.Materials.empty());
        CHECK(data.SubMeshes[0].MaterialIndex == SubMesh::NoMaterial);
    }
}

TEST_CASE("A curved panel's UVs run top-left origin, y-down through the real projection")
{
    const f32 fovY = glm::radians(60.0f);
    constexpr f32 Aspect = 16.0f / 9.0f;
    constexpr f32 Distance = 0.75f;

    const uvec2 grid(8, 6);
    const MeshData data = Primitives::CurvedPanel(vec2(1.7f, 0.96f), 1.0f, grid);
    const CameraView view = PanelView(fovY, Aspect, Distance);

    const auto screen = [&](const u32 column, const u32 row)
    {
        const optional<vec2> projected =
            ProjectToScreen(view, data.Vertices[row * (grid.x + 1) + column].Position, Extent);
        REQUIRE(projected.has_value());
        return *projected;
    };

    // The mapping is monotone in both axes: u increases to the right of the screen and v increases
    // downward. Checked through ProjectToScreen, so a vertically mirrored generator fails here
    // rather than cancelling against its own inverse.
    for (u32 row = 0; row <= grid.y; ++row)
    {
        for (u32 column = 1; column <= grid.x; ++column)
        {
            CHECK(screen(column, row).x > screen(column - 1, row).x);
        }
    }
    for (u32 column = 0; column <= grid.x; ++column)
    {
        for (u32 row = 1; row <= grid.y; ++row)
        {
            CHECK(screen(column, row).y > screen(column, row - 1).y);
        }
    }

    // And the UV origin is the corner a document lays its own origin out at: the top left of the
    // screen, both coordinates below centre.
    CHECK(data.Vertices.front().UV == vec2(0.0f, 0.0f));
    CHECK(screen(0, 0).x < Extent.x * 0.5f);
    CHECK(screen(0, 0).y < Extent.y * 0.5f);
    CHECK(screen(grid.x, grid.y).x > Extent.x * 0.5f);
    CHECK(screen(grid.x, grid.y).y > Extent.y * 0.5f);
}

TEST_CASE("CurvedPanelHit inverts the generator's parameterization")
{
    struct Case
    {
        vec2 Size;
        f32 CurvatureRadius;
        vec3 Eye;
    };

    // The eye inside the cylinder (one root, the exit); the eye outside it, more than two radii from
    // the axis, where the near root is the far flank and every panel hit is the far root; an
    // off-centre eye, so a symmetry cannot hide a swapped axis; and a near-flat panel.
    const std::array<Case, 4> cases{
        Case{.Size = vec2(1.7f, 0.96f), .CurvatureRadius = 1.0f, .Eye = vec3(0.0f, 0.0f, 0.75f)},
        Case{.Size = vec2(1.0f, 0.6f), .CurvatureRadius = 0.5f, .Eye = vec3(0.0f, 0.0f, 3.0f)},
        Case{.Size = vec2(1.7f, 0.96f), .CurvatureRadius = 1.0f, .Eye = vec3(0.2f, 0.15f, 0.8f)},
        Case{.Size = vec2(1.7f, 0.96f), .CurvatureRadius = 1000.0f, .Eye = vec3(0.0f, 0.0f, 0.75f)},
    };

    for (const Case& test : cases)
    {
        CAPTURE(test.CurvatureRadius);
        CAPTURE(test.Eye.z);

        const MeshData data =
            Primitives::CurvedPanel(test.Size, test.CurvatureRadius, uvec2(24, 6));

        for (const CanonicalVertex& vertex : data.Vertices)
        {
            const optional<vec2> hit = Primitives::CurvedPanelHit(
                test.Size, test.CurvatureRadius, test.Eye, vertex.Position - test.Eye);

            // A vertex on the panel's boundary sits exactly on the extent test, which one ulp of the
            // reconstructed intersection can push either side of, so a boundary miss is tolerated
            // here — a boundary hit still has to agree, every interior vertex has to hit, and the
            // near-edge behaviour is pinned at a definite inset by the next case.
            const bool onBoundary = vertex.UV.x <= 0.0f || vertex.UV.x >= 1.0f ||
                                    vertex.UV.y <= 0.0f || vertex.UV.y >= 1.0f;
            if (!hit.has_value() && onBoundary)
            {
                continue;
            }

            CAPTURE(vertex.UV.x);
            CAPTURE(vertex.UV.y);
            REQUIRE(hit.has_value());
            CHECK(glm::length(*hit - vertex.UV) < 1.0e-4f);
        }
    }
}

TEST_CASE("CurvedPanelHit takes the front face, the panel's extent, and the far root")
{
    const vec2 size(1.7f, 0.96f);
    constexpr f32 Radius = 1.0f;
    const vec3 eye(0.0f, 0.0f, 0.75f);

    const auto hitAt = [&](const vec3 origin, const vec2 uv)
    {
        return Primitives::CurvedPanelHit(size, Radius, origin,
                                          SurfacePoint(size, Radius, uv) - origin);
    };

    // Just inside each edge hits, and lands where it was aimed.
    for (const vec2 uv : {vec2(0.001f, 0.5f), vec2(0.999f, 0.5f), vec2(0.5f, 0.001f),
                          vec2(0.5f, 0.999f), vec2(0.001f, 0.999f)})
    {
        const optional<vec2> hit = hitAt(eye, uv);
        CAPTURE(uv.x);
        CAPTURE(uv.y);
        REQUIRE(hit.has_value());
        CHECK(glm::length(*hit - uv) < 1.0e-4f);
    }

    // Just outside any edge misses: past the arc on either flank, and past the height either way.
    for (const vec2 uv :
         {vec2(-0.002f, 0.5f), vec2(1.002f, 0.5f), vec2(0.5f, -0.002f), vec2(0.5f, 1.002f)})
    {
        CAPTURE(uv.x);
        CAPTURE(uv.y);
        CHECK_FALSE(hitAt(eye, uv).has_value());
    }

    // A ray onto the panel's back misses. From behind, the panel's centre is the nearer root and it
    // is back-facing; the far root is half a turn around the cylinder, well outside the arc.
    CHECK_FALSE(
        Primitives::CurvedPanelHit(size, Radius, vec3(0.0f, 0.0f, -2.0f), vec3(0.0f, 0.0f, 1.0f))
            .has_value());

    // A ray along the axis of curvature meets nothing.
    CHECK_FALSE(Primitives::CurvedPanelHit(size, Radius, eye, vec3(0.0f, 1.0f, 0.0f)).has_value());

    // A ray that leaves the cylinder outside the arc misses, rather than clamping to an edge.
    CHECK_FALSE(Primitives::CurvedPanelHit(size, Radius, eye, vec3(1.0f, 0.0f, 0.0f)).has_value());

    // The far-root case, which the naive nearer-root solve gets wrong. From three radii out the eye
    // is outside the cylinder, so a ray at the panel enters the surface at the far flank — half a
    // turn from the panel, outside its arc — and reaches the panel only on the second root.
    // That entry point is on the same cylinder at half a turn from the panel — (0, 0, 2 * Radius) on
    // this ray — so a nearer-root solve reports a miss where the panel is squarely in the way.
    const vec3 farEye(0.0f, 0.0f, 3.0f);
    const optional<vec2> centre =
        Primitives::CurvedPanelHit(size, Radius, farEye, vec3(0.0f, 0.0f, 0.0f) - farEye);
    REQUIRE(centre.has_value());
    CHECK(glm::length(*centre - vec2(0.5f)) < 1.0e-4f);
}

TEST_CASE("CurvedPanelSizeForRect puts a panel's edges on a screen rect's edge rays")
{
    const f32 fovY = glm::radians(60.0f);
    constexpr f32 Aspect = 16.0f / 9.0f;
    constexpr f32 Distance = 0.75f;
    const vec2 rectSize(0.85f, 0.60f);

    const CameraView view = PanelView(fovY, Aspect, Distance);
    const uvec2 grid(32, 8);

    // R < d/2 (the eye outside the cylinder), R < d, R = d, R > d, and effectively flat.
    for (const f32 radius : {0.35f, 0.5f, 0.75f, 1.0f, 1.5f, 5.0f, 1000.0f})
    {
        CAPTURE(radius);
        const vec2 size =
            Primitives::CurvedPanelSizeForRect(fovY, Aspect, rectSize, Distance, radius);
        REQUIRE(std::isfinite(size.x));
        REQUIRE(std::isfinite(size.y));

        const MeshData data = Primitives::CurvedPanel(size, radius, grid);
        const u32 stride = grid.x + 1;

        const vec2 rectMin = (vec2(0.5f) - rectSize * 0.5f) * Extent;
        const vec2 rectMax = (vec2(0.5f) + rectSize * 0.5f) * Extent;

        for (u32 row = 0; row <= grid.y; ++row)
        {
            // The flank columns sit on the rect's left and right edge rays at every height: the
            // horizontal solve is independent of y because the panel is straight along +Y.
            const optional<vec2> left =
                ProjectToScreen(view, data.Vertices[row * stride].Position, Extent);
            const optional<vec2> right =
                ProjectToScreen(view, data.Vertices[row * stride + grid.x].Position, Extent);
            REQUIRE(left.has_value());
            REQUIRE(right.has_value());
            CHECK(std::abs(left->x - rectMin.x) < 0.05f);
            CHECK(std::abs(right->x - rectMax.x) < 0.05f);
        }

        // The height is the flat solve, exact at the centre column. Away from it the surface bows
        // toward the eye and the top and bottom edges project outside the rect — the bow is the
        // geometry, not an error, so the vertical check belongs on the centre column alone.
        const u32 centre = grid.x / 2;
        const optional<vec2> top = ProjectToScreen(view, data.Vertices[centre].Position, Extent);
        const optional<vec2> bottom =
            ProjectToScreen(view, data.Vertices[grid.y * stride + centre].Position, Extent);
        REQUIRE(top.has_value());
        REQUIRE(bottom.has_value());
        CHECK(std::abs(top->y - rectMin.y) < 0.05f);
        CHECK(std::abs(bottom->y - rectMax.y) < 0.05f);
    }

    // A curvature so tight the edge ray misses the cylinder clamps to the silhouette rather than
    // emitting a NaN: the panel's flank is edge-on to the eye there.
    const f32 tangentEdge = rectSize.x * Aspect * std::tan(fovY * 0.5f);
    const f32 sine = std::sin(std::atan(tangentEdge));
    const f32 closing = Distance * sine / (1.0f + sine);

    const vec2 clamped =
        Primitives::CurvedPanelSizeForRect(fovY, Aspect, rectSize, Distance, closing * 0.5f);
    REQUIRE(std::isfinite(clamped.x));
    const f32 clampedRadius = closing * 0.5f;
    const MeshData clampedPanel = Primitives::CurvedPanel(clamped, clampedRadius, uvec2(8, 1));
    const vec3 eye(0.0f, 0.0f, Distance);
    const CanonicalVertex& flank = clampedPanel.Vertices[8];
    CHECK(flank.UV == vec2(1.0f, 0.0f));
    CHECK(glm::dot(glm::normalize(flank.Position - eye), flank.Normal) ==
          doctest::Approx(0.0f).epsilon(1e-3f));

    // The clamp meets the solved branch at the closing radius: the solution's own arc runs out to the
    // silhouette exactly there, so the two branches agree rather than stepping.
    const f32 justInside =
        Primitives::CurvedPanelSizeForRect(fovY, Aspect, rectSize, Distance, closing * 0.999f).x;
    const f32 justOutside =
        Primitives::CurvedPanelSizeForRect(fovY, Aspect, rectSize, Distance, closing * 1.001f).x;
    CHECK(justInside == doctest::Approx(justOutside).epsilon(0.05f));
}

TEST_CASE("A curved panel converges on Plane's geometry as its radius grows")
{
    const vec2 size(1.7f, 0.96f);
    const uvec2 grid(8, 6);
    const MeshData flat = Primitives::Plane(size, grid);

    f32 previousSagitta = size.x;
    for (const f32 radius : {1.0e2f, 1.0e3f, 1.0e4f, 1.0e5f})
    {
        CAPTURE(radius);
        const MeshData data = Primitives::CurvedPanel(size, radius, grid);
        REQUIRE(data.Vertices.size() == flat.Vertices.size());

        // The normals fan out over the panel's own half-arc and no further, so this bound tightens
        // toward +Z everywhere as the radius grows.
        const f32 halfAngle = size.x * 0.5f / radius;

        f32 sagitta = 0.0f;
        for (usize i = 0; i < data.Vertices.size(); ++i)
        {
            const CanonicalVertex& panel = data.Vertices[i];
            const CanonicalVertex& plane = flat.Vertices[i];

            REQUIRE(std::isfinite(panel.Position.x));
            REQUIRE(std::isfinite(panel.Position.y));
            REQUIRE(std::isfinite(panel.Position.z));

            // Plane is the same grid in XZ with a +Y normal, so its in-plane coordinates are the
            // panel's with the vertical axis relabelled — v runs +Z on one and -Y on the other.
            CHECK(panel.UV == plane.UV);
            CHECK(panel.Position.x == doctest::Approx(plane.Position.x).epsilon(1e-4f));
            CHECK(panel.Position.y == doctest::Approx(-plane.Position.z).epsilon(1e-4f));
            CHECK(glm::length(panel.Normal - vec3(0.0f, 0.0f, 1.0f)) <= halfAngle);

            sagitta = glm::max(sagitta, std::abs(panel.Position.z));
        }

        // The deviation from flat is the sagitta and it falls as size.x^2 / (8 * radius).
        CHECK(sagitta == doctest::Approx(size.x * size.x / (8.0f * radius)).epsilon(1e-3f));
        CHECK(sagitta < previousSagitta);
        previousSagitta = sagitta;
    }
}
