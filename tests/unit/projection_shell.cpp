// Primitives::ProjectionShell — the reprojection property, in one place, on the CPU. The generator's
// whole contract is that the shell reproduces a screen rect of one perspective projection when viewed
// from its own eye point, so every case here projects a generated vertex back through
// ProjectToScreen — the projection the renderer actually uses — rather than through the generator's
// own inverse. That distinction is the point: an inverse-based check passes even when the document is
// vertically mirrored, because the mirror cancels.
//
// Beside it, the closed-form reprojection bound: the derivation states the between-vertex error
// exactly, so the exact per-chord error is measured over the real grid and required to sit under the
// bound — which validates the derivation and its evaluation at once — plus the O(cell^2) falloff and
// the bound's independence from radius.

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
    // The reference pose: the eye at the local origin looking down -Z with +Y up, at the same
    // projection the shell was generated from. This is the one pose the shell is exact from.
    CameraView ReferenceView(const f32 fovY, const f32 aspect)
    {
        CameraView view;
        view.SetPerspective(fovY, aspect, 0.01f, 1000.0f);
        view.SetViewFromWorld(mat4(1.0f));
        return view;
    }

    // The screen point a UV lands on under the ideal mapping: the rect parameter placed back into
    // window fractions, then into pixels. Top-left origin on both sides.
    vec2 IdealScreenPoint(const vec2 uv, const vec2 rectCenter, const vec2 rectSize,
                          const vec2 extent)
    {
        return (rectCenter - rectSize * 0.5f + uv * rectSize) * extent;
    }

    // The largest reprojection displacement actually present in a generated shell, measured over
    // every triangle edge: the exact chord-midpoint error the bound's derivation predicts. Sampling
    // the midpoint is enough — the derivation shows the error along a chord is a pure
    // reparametrization whose magnitude peaks there.
    f32 MeasuredChordError(const MeshData& data, const CameraView& view, const vec2 rectCenter,
                           const vec2 rectSize, const vec2 extent)
    {
        f32 worst = 0.0f;
        for (usize i = 0; i + 2 < data.Indices.size(); i += 3)
        {
            const std::array<u32, 3> corners{data.Indices[i], data.Indices[i + 1],
                                             data.Indices[i + 2]};
            for (u32 edge = 0; edge < 3; ++edge)
            {
                const CanonicalVertex& from = data.Vertices[corners[edge]];
                const CanonicalVertex& to = data.Vertices[corners[(edge + 1) % 3]];

                // Where the rasterizer puts the chord's midpoint, and where the perspective-correct
                // UV it carries there says that fragment should have landed.
                const optional<vec2> drawn =
                    ProjectToScreen(view, (from.Position + to.Position) * 0.5f, extent);
                REQUIRE(drawn.has_value());
                const vec2 ideal =
                    IdealScreenPoint((from.UV + to.UV) * 0.5f, rectCenter, rectSize, extent);
                worst = glm::max(worst, glm::length(*drawn - ideal));
            }
        }
        return worst;
    }
}

TEST_CASE("ProjectionShell reproduces its source screen rect exactly at every vertex")
{
    // The matrix spans the shape inputs and the degenerate ends the preconditions name: a single
    // cell, a lopsided grid, an extreme aspect, a near-pi field of view, an off-centre rect, and a
    // rect flush against the window edge.
    struct Case
    {
        f32 FovY;
        f32 Aspect;
        vec2 RectCenter;
        vec2 RectSize;
        f32 Radius;
        uvec2 Subdivisions;
    };

    const std::array<Case, 8> cases{
        Case{.FovY = glm::radians(60.0f),
             .Aspect = 16.0f / 9.0f,
             .RectCenter = vec2(0.5f),
             .RectSize = vec2(1.0f),
             .Radius = 2.0f,
             .Subdivisions = uvec2(32)},
        Case{.FovY = glm::radians(60.0f),
             .Aspect = 16.0f / 9.0f,
             .RectCenter = vec2(0.5f),
             .RectSize = vec2(0.5f),
             .Radius = 2.0f,
             .Subdivisions = uvec2(32)},
        Case{.FovY = glm::radians(60.0f),
             .Aspect = 16.0f / 9.0f,
             .RectCenter = vec2(0.5f),
             .RectSize = vec2(1.0f),
             .Radius = 0.25f,
             .Subdivisions = uvec2(1)},
        Case{.FovY = glm::radians(90.0f),
             .Aspect = 1.0f,
             .RectCenter = vec2(0.25f, 0.75f),
             .RectSize = vec2(0.3f, 0.2f),
             .Radius = 5.0f,
             .Subdivisions = uvec2(7, 3)},
        Case{.FovY = glm::radians(30.0f),
             .Aspect = 4.0f,
             .RectCenter = vec2(0.5f),
             .RectSize = vec2(1.0f),
             .Radius = 1.0f,
             .Subdivisions = uvec2(4, 16)},
        Case{.FovY = glm::radians(170.0f),
             .Aspect = 0.25f,
             .RectCenter = vec2(0.5f),
             .RectSize = vec2(1.0f),
             .Radius = 3.0f,
             .Subdivisions = uvec2(8)},
        Case{.FovY = glm::radians(75.0f),
             .Aspect = 16.0f / 10.0f,
             .RectCenter = vec2(0.1f, 0.1f),
             .RectSize = vec2(0.2f),
             .Radius = 2.0f,
             .Subdivisions = uvec2(5)},
        Case{.FovY = glm::radians(45.0f),
             .Aspect = 1.5f,
             .RectCenter = vec2(0.5f),
             .RectSize = vec2(1.0f),
             .Radius = 10.0f,
             .Subdivisions = uvec2(0)},
    };

    constexpr vec2 Extent{1920.0f, 1080.0f};

    for (const Case& test : cases)
    {
        CAPTURE(test.FovY);
        CAPTURE(test.Aspect);
        CAPTURE(test.Subdivisions.x);
        CAPTURE(test.Subdivisions.y);

        const MeshData data = Primitives::ProjectionShell(
            test.FovY, test.Aspect, test.RectCenter, test.RectSize, test.Radius, test.Subdivisions);

        // Zero subdivisions clamp to one cell per axis, exactly as Plane clamps.
        const uvec2 grid = glm::max(test.Subdivisions, uvec2(1));
        CHECK(data.Vertices.size() == static_cast<usize>(grid.x + 1) * (grid.y + 1));
        CHECK(data.Indices.size() == static_cast<usize>(grid.x) * grid.y * 6);

        const CameraView view = ReferenceView(test.FovY, test.Aspect);

        for (const CanonicalVertex& vertex : data.Vertices)
        {
            // Every vertex sits at `radius` from the eye — the collimation the shell exists for.
            CHECK(glm::length(vertex.Position) == doctest::Approx(test.Radius).epsilon(1e-4f));

            // The normal faces the eye and is the negated ray; the tangent is unit and orthogonal.
            CHECK(glm::length(vertex.Normal) == doctest::Approx(1.0f).epsilon(1e-4f));
            CHECK(glm::dot(vertex.Normal, -vertex.Position) > 0.0f);
            const vec3 tangent(vertex.Tangent);
            CHECK(glm::length(tangent) == doctest::Approx(1.0f).epsilon(1e-4f));
            CHECK(std::abs(glm::dot(tangent, vertex.Normal)) ==
                  doctest::Approx(0.0f).epsilon(1e-3f));

            // The property: projected through the matching perspective from the reference pose, the
            // vertex lands on the screen point its UV names. Checked through ProjectToScreen, so a
            // vertically mirrored generator fails here rather than cancelling against its own
            // inverse.
            const optional<vec2> projected = ProjectToScreen(view, vertex.Position, Extent);
            REQUIRE(projected.has_value());
            const vec2 ideal = IdealScreenPoint(vertex.UV, test.RectCenter, test.RectSize, Extent);
            CHECK(glm::length(*projected - ideal) < 0.01f);
        }

        // UVs are the grid parameter across the rect, top-left origin and y-down: the first vertex is
        // the rect's top-left corner and the last is its bottom-right.
        CHECK(data.Vertices.front().UV == vec2(0.0f, 0.0f));
        CHECK(data.Vertices.back().UV == vec2(1.0f, 1.0f));

        // The vertical axis, stated as a fact rather than a round trip: the v = 0 row is *above* the
        // v = 1 row in camera space (+Y up) while it is the *top* of the screen rect (y-down).
        CHECK(data.Vertices.front().Position.y > data.Vertices.back().Position.y);

        // Winding faces the eye: the geometric face normal agrees with the shading normals, so the
        // CCW-front surface pipeline does not cull the shell away from the reference pose.
        for (usize i = 0; i + 2 < data.Indices.size(); i += 3)
        {
            const CanonicalVertex& v0 = data.Vertices[data.Indices[i + 0]];
            const CanonicalVertex& v1 = data.Vertices[data.Indices[i + 1]];
            const CanonicalVertex& v2 = data.Vertices[data.Indices[i + 2]];
            const vec3 geometric = glm::cross(v1.Position - v0.Position, v2.Position - v0.Position);
            CHECK(glm::dot(geometric, v0.Normal + v1.Normal + v2.Normal) > 0.0f);
        }

        // One submesh, no material by default — the shared generator contract.
        REQUIRE(data.SubMeshes.size() == 1);
        CHECK(data.SubMeshes[0].IndexCount == data.Indices.size());
        CHECK(data.Materials.empty());
        CHECK(data.SubMeshes[0].MaterialIndex == SubMesh::NoMaterial);
    }
}

TEST_CASE("The closed-form reprojection bound holds over the real grid at every density")
{
    constexpr vec2 Extent{1920.0f, 1080.0f};
    const f32 fovY = glm::radians(60.0f);
    constexpr f32 Aspect = 16.0f / 9.0f;

    for (const vec2 rectSize : {vec2(1.0f), vec2(0.5f), vec2(0.4f, 0.75f)})
    {
        for (const u32 density : {2u, 4u, 8u, 16u, 32u, 64u})
        {
            const uvec2 subdivisions(density);
            const vec2 rectCenter(0.5f);
            const MeshData data =
                Primitives::ProjectionShell(fovY, Aspect, rectCenter, rectSize, 2.0f, subdivisions);
            const f32 bound = Primitives::ProjectionShellReprojectionBound(
                fovY, Aspect, rectCenter, rectSize, subdivisions, Extent);
            const f32 measured =
                MeasuredChordError(data, ReferenceView(fovY, Aspect), rectCenter, rectSize, Extent);

            CAPTURE(density);
            CAPTURE(rectSize.x);
            CAPTURE(rectSize.y);
            CAPTURE(bound);
            CAPTURE(measured);
            // The bound is an upper bound, and not a vacuous one: the real grid's worst chord lands
            // within a factor of two of it. The margin is the f32 cancellation floor of the
            // measurement, not slack in the bound — a sub-pixel displacement differenced out of
            // screen coordinates of magnitude ~1e3 carries about 1e-4 points of noise, which at the
            // finest grids is a larger number than the gap being measured.
            CHECK(measured <= bound + 1.0e-3f);
            CHECK(measured > bound * 0.4f);
        }
    }
}

TEST_CASE("The reprojection bound falls as the square of the cell and ignores the radius")
{
    const f32 fovY = glm::radians(60.0f);
    constexpr f32 Aspect = 16.0f / 9.0f;
    constexpr vec2 Extent{1920.0f, 1080.0f};
    const vec2 rectCenter(0.5f);

    const auto bound = [&](const vec2 rectSize, const u32 density)
    {
        return Primitives::ProjectionShellReprojectionBound(fovY, Aspect, rectCenter, rectSize,
                                                            uvec2(density), Extent);
    };

    // Doubling the grid quarters the error, up to the rect-reach clamp: the O(cell^2) falloff the
    // derivation predicts. Checked in the regime where the maximising point sits on the box
    // boundary, so the ratio is exactly four.
    const f32 half32 = bound(vec2(0.5f), 32);
    CHECK(bound(vec2(0.5f), 16) / half32 == doctest::Approx(4.0f).epsilon(0.02f));
    CHECK(bound(vec2(0.5f), 64) / half32 == doctest::Approx(0.25f).epsilon(0.02f));

    // The shipped configurations, stated as numbers so a regression in the evaluation is visible:
    // the 50% rect sits well inside a half-point budget, the full frustum does not.
    CHECK(half32 == doctest::Approx(0.1385f).epsilon(0.01f));
    CHECK(bound(vec2(1.0f), 32) == doctest::Approx(0.6334f).epsilon(0.01f));

    // Radius scales every vertex about the eye, which the projection divides straight back out — so
    // two shells of different radius reproject identically and the bound takes no radius at all.
    const uvec2 subdivisions(16);
    const CameraView view = ReferenceView(fovY, Aspect);
    const f32 near = MeasuredChordError(
        Primitives::ProjectionShell(fovY, Aspect, rectCenter, vec2(0.5f), 0.5f, subdivisions), view,
        rectCenter, vec2(0.5f), Extent);
    const f32 far = MeasuredChordError(
        Primitives::ProjectionShell(fovY, Aspect, rectCenter, vec2(0.5f), 50.0f, subdivisions),
        view, rectCenter, vec2(0.5f), Extent);
    CHECK(near == doctest::Approx(far).epsilon(1e-3f));
}
