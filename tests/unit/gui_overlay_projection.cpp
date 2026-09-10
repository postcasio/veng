// World-anchored GUI overlay projection — the device-free core the pre-bloom overlay pass records
// through. ProjectGuiOverlayPoint places a document point on the overlay's flat virtual plane and
// projects it through the live camera to screen pixels; a point behind the eye is culled. These pin
// the properties the "HUD stays anchored while you look around" behavior reduces to — a known
// surface-local point lands where expected, a camera rotation slides it, and a behind-eye point is
// dropped — plus the screen-space affine mapping the flat placement reproduces. Pure math; no device.

#include <doctest/doctest.h>

#include <cmath>

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
