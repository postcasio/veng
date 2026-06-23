// Viewport window-to-view mapping + the Ray primitive: pure CPU math, no
// Context, no Vulkan symbol touched. The hit-test, the normalized remap (under
// a window offset and at the zero-offset equivalent), and the inverse-view-
// projection unproject are exercised here without a device by computing them
// directly against the same glm::inverse(CameraView::ViewProjection()) the
// Viewport impl uses. Ray::At is pinned alongside.

#include <doctest/doctest.h>

// Veng.h (pulled by these) sets the engine's GLM config — Vulkan ZO depth, the
// Y-flip — so the unproject here matches the Viewport's; it must precede any
// bare glm include.
#include <Veng/Math/Ray.h>
#include <Veng/Renderer/ViewportRegion.h>
#include <Veng/Scene/Camera.h>

#include <glm/gtc/matrix_transform.hpp>

#include <optional>

using namespace Veng;
using Veng::Renderer::ViewportRegion;

namespace
{
    bool VecApprox(const vec3& a, const vec3& b, f32 eps = 1e-4f)
    {
        return glm::all(glm::lessThan(glm::abs(a - b), vec3(eps)));
    }

    bool Vec2Approx(const vec2& a, const vec2& b, f32 eps = 1e-4f)
    {
        return glm::all(glm::lessThan(glm::abs(a - b), vec2(eps)));
    }

    // The Viewport::WindowToViewport math, isolated so the pure-logic suite needs
    // no Context to construct a Viewport. Mirrors the impl exactly.
    optional<vec2> WindowToViewport(const ViewportRegion& region, ivec2 windowPoint)
    {
        if (region.Extent.x == 0 || region.Extent.y == 0)
        {
            return std::nullopt;
        }

        const ivec2 local = windowPoint - region.Offset;
        const ivec2 extent = ivec2(region.Extent);
        if (local.x < 0 || local.y < 0 || local.x >= extent.x || local.y >= extent.y)
        {
            return std::nullopt;
        }

        return vec2(static_cast<f32>(local.x) / static_cast<f32>(extent.x),
                    static_cast<f32>(local.y) / static_cast<f32>(extent.y));
    }

    // The Viewport::ScreenToWorldRay unproject, isolated from the retained-camera
    // plumbing so the math is testable without a Viewport. Mirrors the impl.
    optional<Ray> ScreenToWorldRay(const ViewportRegion& region, const CameraView& camera,
                                   ivec2 windowPoint)
    {
        const optional<vec2> fraction = WindowToViewport(region, windowPoint);
        if (!fraction.has_value())
        {
            return std::nullopt;
        }

        const vec2 ndc = *fraction * 2.0f - 1.0f;
        const mat4 invViewProj = glm::inverse(camera.ViewProjection());
        const vec4 nearClip = invViewProj * vec4(ndc, 0.0f, 1.0f);
        const vec4 farClip = invViewProj * vec4(ndc, 1.0f, 1.0f);
        const vec3 nearWorld = vec3(nearClip) / nearClip.w;
        const vec3 farWorld = vec3(farClip) / farClip.w;

        return Ray{
            .Origin = camera.GetPosition(),
            .Direction = glm::normalize(farWorld - nearWorld),
        };
    }
}

TEST_CASE("Ray::At returns the origin at t=0 and advances along the direction by t")
{
    const Ray ray{.Origin = vec3(1.0f, 2.0f, 3.0f), .Direction = vec3(0.0f, 0.0f, -1.0f)};

    CHECK(VecApprox(ray.At(0.0f), ray.Origin));
    CHECK(VecApprox(ray.At(5.0f), vec3(1.0f, 2.0f, -2.0f)));

    // With a unit direction, At(t) advances exactly t world units.
    CHECK(glm::length(ray.At(5.0f) - ray.Origin) == doctest::Approx(5.0f));
}

TEST_CASE("WindowToViewport hit-tests the region and remaps to normalized [0,1]")
{
    const ViewportRegion region{.Offset = {0, 0}, .Extent = {800, 600}};

    SUBCASE("A point inside maps to its fractional position")
    {
        const optional<vec2> frac = WindowToViewport(region, {200, 150});
        REQUIRE(frac.has_value());
        CHECK(Vec2Approx(*frac, vec2(0.25f, 0.25f)));
    }

    SUBCASE("The region's top-left maps to (0,0)")
    {
        const optional<vec2> frac = WindowToViewport(region, {0, 0});
        REQUIRE(frac.has_value());
        CHECK(Vec2Approx(*frac, vec2(0.0f, 0.0f)));
    }

    SUBCASE("The region's center maps to (0.5,0.5)")
    {
        const optional<vec2> frac = WindowToViewport(region, {400, 300});
        REQUIRE(frac.has_value());
        CHECK(Vec2Approx(*frac, vec2(0.5f, 0.5f)));
    }

    SUBCASE("A point on the exclusive right/bottom edge is outside")
    {
        CHECK_FALSE(WindowToViewport(region, {800, 300}).has_value());
        CHECK_FALSE(WindowToViewport(region, {400, 600}).has_value());
    }

    SUBCASE("A point left/above the region is outside")
    {
        CHECK_FALSE(WindowToViewport(region, {-1, 300}).has_value());
        CHECK_FALSE(WindowToViewport(region, {400, -1}).has_value());
    }

    SUBCASE("A zero-extent region hit-tests to nullopt")
    {
        const ViewportRegion empty{.Offset = {0, 0}, .Extent = {0, 0}};
        CHECK_FALSE(WindowToViewport(empty, {0, 0}).has_value());
    }
}

TEST_CASE("WindowToViewport remaps the same under a non-zero region offset")
{
    // A splitscreen lower-right quadrant: the same normalized coordinate as the
    // equivalent point in a zero-offset region of the same extent.
    const ViewportRegion zeroOffset{.Offset = {0, 0}, .Extent = {640, 360}};
    const ViewportRegion quadrant{.Offset = {640, 360}, .Extent = {640, 360}};

    const optional<vec2> baseFrac = WindowToViewport(zeroOffset, {160, 90});
    const optional<vec2> offsetFrac = WindowToViewport(quadrant, {640 + 160, 360 + 90});

    REQUIRE(baseFrac.has_value());
    REQUIRE(offsetFrac.has_value());
    CHECK(Vec2Approx(*baseFrac, *offsetFrac));
    CHECK(Vec2Approx(*offsetFrac, vec2(0.25f, 0.25f)));

    // A window point inside the zero-offset region but outside the offset quadrant
    // is a miss for the quadrant.
    CHECK_FALSE(WindowToViewport(quadrant, {160, 90}).has_value());
}

TEST_CASE("ScreenToWorldRay unprojects through the retained camera")
{
    // A camera looking down -Z from the origin, the engine's Vulkan-Y-flip
    // projection. The ray origin is always the camera position.
    CameraView camera;
    camera.SetPerspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
    camera.SetView(vec3(0.0f, 0.0f, 5.0f), vec3(0.0f, 0.0f, 0.0f), vec3(0.0f, 1.0f, 0.0f));

    const ViewportRegion region{.Offset = {0, 0}, .Extent = {1600, 900}};

    SUBCASE("A center pixel yields a ray along the camera forward axis")
    {
        const optional<Ray> ray = ScreenToWorldRay(region, camera, {800, 450});
        REQUIRE(ray.has_value());

        CHECK(VecApprox(ray->Origin, vec3(0.0f, 0.0f, 5.0f)));
        // Forward is -Z (the camera looks at the origin from +Z).
        CHECK(VecApprox(ray->Direction, vec3(0.0f, 0.0f, -1.0f)));
        // The producer normalizes the direction.
        CHECK(glm::length(ray->Direction) == doctest::Approx(1.0f));
    }

    SUBCASE("An off-center pixel skews the direction off the forward axis")
    {
        // A pixel to the right of center: the ray still starts at the camera but
        // tilts toward +X (world right) and away from straight -Z.
        const optional<Ray> ray = ScreenToWorldRay(region, camera, {1200, 450});
        REQUIRE(ray.has_value());

        CHECK(VecApprox(ray->Origin, vec3(0.0f, 0.0f, 5.0f)));
        CHECK(ray->Direction.x > 0.0f);
        CHECK(ray->Direction.z < 0.0f);
        CHECK(std::abs(ray->Direction.y) < 1e-4f);
        CHECK(glm::length(ray->Direction) == doctest::Approx(1.0f));
    }

    SUBCASE("A pixel outside the region yields no ray")
    {
        CHECK_FALSE(ScreenToWorldRay(region, camera, {-1, 450}).has_value());
        CHECK_FALSE(ScreenToWorldRay(region, camera, {1600, 450}).has_value());
    }
}
