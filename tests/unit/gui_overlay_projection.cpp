// World-anchored GUI overlay projection — the device-free core the pre-bloom overlay pass records
// through. ProjectGuiOverlayPoint places a document point on the overlay's flat virtual plane and
// projects it through the live camera to screen pixels; a point behind the eye is culled. These pin
// the properties the "HUD stays anchored while you look around" behavior reduces to — a known
// surface-local point lands where expected, a camera rotation slides it, and a behind-eye point is
// dropped — plus the screen-space affine mapping the flat placement reproduces, and the document rect
// a material overlay's intermediate is sized to. Pure math; no device.

#include <doctest/doctest.h>

#include <algorithm>
#include <cmath>
#include <utility>

#include <glm/gtc/matrix_transform.hpp>

#include "Renderer/GuiOverlayProjection.h"

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr vec2 Extent{800.0f, 600.0f};
    constexpr vec2 DocExtent{400.0f, 300.0f};
    constexpr vec2 SurfaceSize{2.0f, 1.5f};

    // A camera at the origin looking down -Z (the plane sits a couple units ahead), y up.
    CameraView ForwardCamera()
    {
        CameraView camera;
        camera.SetPerspective(glm::radians(60.0f), Extent.x / Extent.y, 0.1f, 100.0f);
        camera.SetView(vec3(0.0f), vec3(0.0f, 0.0f, -1.0f), vec3(0.0f, 1.0f, 0.0f));
        return camera;
    }

    // A camera yawed by theta about +Y (still at the origin), so a fixed world point slides on screen.
    CameraView YawedCamera(const f32 theta)
    {
        CameraView camera;
        camera.SetPerspective(glm::radians(60.0f), Extent.x / Extent.y, 0.1f, 100.0f);
        camera.SetView(vec3(0.0f), vec3(std::sin(theta), 0.0f, -std::cos(theta)),
                       vec3(0.0f, 1.0f, 0.0f));
        return camera;
    }
}

TEST_CASE("gui overlay projection: the document center lands at the screen center")
{
    // A plane two units directly ahead, centered on the view axis: its document center projects to
    // the middle of the target.
    const mat4 model =
        ComputeGuiOverlayModel(vec3(0.0f, 0.0f, -2.0f), quat(1.0f, 0.0f, 0.0f, 0.0f));
    const optional<vec2> screen = ProjectGuiOverlayPoint(DocExtent * 0.5f, DocExtent, SurfaceSize,
                                                         model, ForwardCamera(), Extent);
    REQUIRE(screen.has_value());
    CHECK(screen->x == doctest::Approx(Extent.x * 0.5f));
    CHECK(screen->y == doctest::Approx(Extent.y * 0.5f));
}

TEST_CASE("gui overlay projection: a surface-local offset projects to the matching screen side")
{
    const mat4 model =
        ComputeGuiOverlayModel(vec3(0.0f, 0.0f, -2.0f), quat(1.0f, 0.0f, 0.0f, 0.0f));
    const CameraView camera = ForwardCamera();
    const vec2 center = Extent * 0.5f;

    // The document's right edge sits at +SurfaceSize.x/2 on the plane, so it projects right of
    // center; its bottom edge (document y down) sits lower on screen.
    const optional<vec2> right = ProjectGuiOverlayPoint(
        {DocExtent.x, DocExtent.y * 0.5f}, DocExtent, SurfaceSize, model, camera, Extent);
    const optional<vec2> bottom = ProjectGuiOverlayPoint(
        {DocExtent.x * 0.5f, DocExtent.y}, DocExtent, SurfaceSize, model, camera, Extent);
    REQUIRE(right.has_value());
    REQUIRE(bottom.has_value());
    CHECK(right->x > center.x);
    CHECK(right->y == doctest::Approx(center.y));
    CHECK(bottom->y > center.y);
    CHECK(bottom->x == doctest::Approx(center.x));
}

TEST_CASE("gui overlay projection: rotating the camera slides the anchored point monotonically")
{
    const mat4 model =
        ComputeGuiOverlayModel(vec3(0.0f, 0.0f, -2.0f), quat(1.0f, 0.0f, 0.0f, 0.0f));
    const vec2 docCenter = DocExtent * 0.5f;

    const optional<vec2> straight =
        ProjectGuiOverlayPoint(docCenter, DocExtent, SurfaceSize, model, YawedCamera(0.0f), Extent);
    const optional<vec2> small = ProjectGuiOverlayPoint(docCenter, DocExtent, SurfaceSize, model,
                                                        YawedCamera(0.15f), Extent);
    const optional<vec2> large = ProjectGuiOverlayPoint(docCenter, DocExtent, SurfaceSize, model,
                                                        YawedCamera(0.30f), Extent);
    REQUIRE(straight.has_value());
    REQUIRE(small.has_value());
    REQUIRE(large.has_value());

    // Straight ahead is centered; yawing the camera one way slides the fixed point the other way,
    // and further yaw slides it further — the look-around slide, monotone in the angle.
    CHECK(straight->x == doctest::Approx(Extent.x * 0.5f));
    CHECK(small->x < straight->x);
    CHECK(large->x < small->x);
    // Yaw about the horizon leaves the vertical position put.
    CHECK(small->y == doctest::Approx(straight->y));
}

TEST_CASE("gui overlay projection: a point behind the eye is culled")
{
    // The same plane placed behind the camera (positive Z, the camera looks down -Z): every point
    // is behind the eye, so the projection reports a cull.
    const mat4 model = ComputeGuiOverlayModel(vec3(0.0f, 0.0f, 2.0f), quat(1.0f, 0.0f, 0.0f, 0.0f));
    const optional<vec2> screen = ProjectGuiOverlayPoint(DocExtent * 0.5f, DocExtent, SurfaceSize,
                                                         model, ForwardCamera(), Extent);
    CHECK_FALSE(screen.has_value());
}

TEST_CASE("gui overlay projection: the model transform places a local point in world space")
{
    // ComputeGuiOverlayModel is translate-then-rotate: a 90° yaw about +Y sends the plane's local +X
    // toward -Z, then the translation offsets it.
    const quat yaw = glm::angleAxis(glm::radians(90.0f), vec3(0.0f, 1.0f, 0.0f));
    const mat4 model = ComputeGuiOverlayModel(vec3(5.0f, 0.0f, 0.0f), yaw);
    const vec3 world = vec3(model * vec4(1.0f, 0.0f, 0.0f, 1.0f));
    CHECK(world.x == doctest::Approx(5.0f));
    CHECK(world.y == doctest::Approx(0.0f));
    CHECK(world.z == doctest::Approx(-1.0f));
}

TEST_CASE(
    "gui overlay document rect: covers the projection, granule-aligned, clamped to the target")
{
    constexpr uvec2 Target{1000, 700};
    constexpr u32 G = GuiOverlayDocumentGranule;

    // Bounds inside, straddling each edge, sub-pixel, and covering the whole target — the boundary
    // cases a projected pane reaches while it swings across the screen.
    const std::pair<vec2, vec2> bounds[] = {
        {vec2(130.4f, 90.2f), vec2(300.6f, 211.9f)},
        {vec2(-40.0f, 20.0f), vec2(70.0f, 80.0f)},
        {vec2(950.5f, 650.0f), vec2(1100.0f, 760.0f)},
        {vec2(500.25f, 333.75f), vec2(500.75f, 334.25f)},
        {vec2(-10.0f), vec2(2000.0f)},
        {vec2(64.0f, 128.0f), vec2(128.0f, 192.0f)},
    };

    u32 failures = 0;
    for (const auto& [lo, hi] : bounds)
    {
        const GuiOverlayDocumentRect rect = ComputeGuiOverlayDocumentRect(lo, hi, Target);
        const uvec2 end = rect.Origin + rect.Size;
        const vec2 coveredLo = glm::max(glm::floor(lo), vec2(0.0f));
        const vec2 coveredHi = glm::min(glm::ceil(hi), vec2(Target));
        const bool contains = vec2(rect.Origin).x <= coveredLo.x &&
                              vec2(rect.Origin).y <= coveredLo.y && vec2(end).x >= coveredHi.x &&
                              vec2(end).y >= coveredHi.y;
        const bool clamped = end.x <= Target.x && end.y <= Target.y;
        const bool aligned = rect.Origin.x % G == 0 && rect.Origin.y % G == 0 &&
                             (end.x % G == 0 || end.x == Target.x) &&
                             (end.y % G == 0 || end.y == Target.y);
        failures += (rect.IsEmpty() || !contains || !clamped || !aligned) ? 1u : 0u;
    }
    CHECK(failures == 0);

    // Already-aligned bounds take exactly their own granules.
    const GuiOverlayDocumentRect exact =
        ComputeGuiOverlayDocumentRect(vec2(64.0f, 128.0f), vec2(128.0f, 192.0f), Target);
    CHECK(exact.Origin == uvec2(64, 128));
    CHECK(exact.Size == uvec2(64, 64));

    // A projection wholly off the target, on any side, covers nothing.
    CHECK(ComputeGuiOverlayDocumentRect(vec2(-200.0f), vec2(-1.0f), Target).IsEmpty());
    CHECK(
        ComputeGuiOverlayDocumentRect(vec2(1000.0f, 0.0f), vec2(1200.0f, 50.0f), Target).IsEmpty());
    CHECK(ComputeGuiOverlayDocumentRect(vec2(0.0f, 900.0f), vec2(50.0f, 950.0f), Target).IsEmpty());
}

TEST_CASE(
    "gui overlay document rect: the intermediate grows to cover and never shrinks while active")
{
    constexpr uvec2 Target{1000, 700};

    // A first allocation is one granule, then grows to cover a larger rect, rounded to the granule.
    const uvec2 first = GrowGuiOverlayDocumentAllocation(uvec2(0), uvec2(0), Target);
    CHECK(first == uvec2(GuiOverlayDocumentGranule));
    const uvec2 grown = GrowGuiOverlayDocumentAllocation(first, uvec2(300, 130), Target);
    CHECK(grown == uvec2(320, 192));

    // A smaller rect, or one larger on a single axis, never takes back what the other axis holds.
    CHECK(GrowGuiOverlayDocumentAllocation(grown, uvec2(10, 10), Target) == grown);
    CHECK(GrowGuiOverlayDocumentAllocation(grown, uvec2(100, 400), Target) == uvec2(320, 448));

    // The target's own granule-rounded extent bounds it, and a target resized below the high-water
    // mark brings it down to the new bound.
    CHECK(GrowGuiOverlayDocumentAllocation(grown, Target, Target) == uvec2(1024, 704));
    CHECK(GrowGuiOverlayDocumentAllocation(uvec2(1024, 704), uvec2(10), uvec2(200, 100)) ==
          uvec2(256, 128));
}

TEST_CASE("gui overlay projection: the homography agrees with the per-point projection")
{
    // A plane tilted on two axes and off the view axis, so the map is genuinely projective: any point
    // of the document, taken through the homography, lands where the vertex projection puts it.
    const quat tilt = glm::angleAxis(glm::radians(25.0f), glm::normalize(vec3(1.0f, 0.6f, 0.0f)));
    const mat4 model = ComputeGuiOverlayModel(vec3(0.4f, -0.2f, -2.5f), tilt);
    const CameraView camera = ForwardCamera();
    const mat3 homography =
        ComputeGuiOverlayHomography(DocExtent, SurfaceSize, model, camera, Extent);

    f32 worst = 0.0f;
    for (const vec2 share :
         {vec2(0.0f), vec2(1.0f, 0.0f), vec2(0.0f, 1.0f), vec2(1.0f), vec2(0.5f), vec2(0.2f, 0.7f)})
    {
        const vec2 point = share * DocExtent;
        const optional<vec2> projected =
            ProjectGuiOverlayPoint(point, DocExtent, SurfaceSize, model, camera, Extent);
        REQUIRE(projected.has_value());
        worst =
            std::max(worst, glm::length(ApplyGuiOverlayHomography(homography, point) - *projected));
    }
    CHECK(worst < 1e-3f);
}

TEST_CASE("gui overlay projection: the inverse homography returns a pixel to its document point")
{
    const quat tilt = glm::angleAxis(glm::radians(-30.0f), vec3(0.0f, 1.0f, 0.0f));
    const mat4 model = ComputeGuiOverlayModel(vec3(-0.3f, 0.1f, -2.0f), tilt);
    const mat3 homography =
        ComputeGuiOverlayHomography(DocExtent, SurfaceSize, model, ForwardCamera(), Extent);
    const vec2 point(123.0f, 77.0f);
    const vec2 back = ApplyGuiOverlayHomography(glm::inverse(homography),
                                                ApplyGuiOverlayHomography(homography, point));
    CHECK(back.x == doctest::Approx(point.x).epsilon(1e-4));
    CHECK(back.y == doctest::Approx(point.y).epsilon(1e-4));
}

TEST_CASE("gui overlay projection: a screen-space homography is the stretch to the target")
{
    const mat3 homography = ComputeGuiOverlayScreenHomography(DocExtent, Extent);
    const vec2 corner = ApplyGuiOverlayHomography(homography, DocExtent);
    CHECK(corner.x == doctest::Approx(Extent.x));
    CHECK(corner.y == doctest::Approx(Extent.y));
}
