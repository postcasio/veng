// Frustum extraction + AABB intersection: pure CPU math, no Context, no Vulkan
// symbol touched. Pins the Gribb-Hartmann extraction against the engine's real
// Y-flipped Camera::ViewProjection() (Vulkan ZO, not a bare glm::perspective),
// the ZO near-plane discriminator that separates the Vulkan form from the GL
// form, the conservative p-vertex AABB test, and ortho extraction.

#include <doctest/doctest.h>

// Veng.h (pulled by these) sets the engine's GLM config — Vulkan ZO depth, the
// Y-flip handedness — and must precede any bare glm include so glm compiles with
// it. The bounds/camera tests rely on the same ZO config the engine uses.
#include <Veng/Math/AABB.h>
#include <Veng/Math/Frustum.h>
#include <Veng/Scene/Camera.h>

#include <glm/gtc/matrix_transform.hpp>

using namespace Veng;

namespace
{
    // A point is inside the frustum when it is on the inward side of all six
    // planes — the same predicate Intersects collapses for a box.
    bool PointInside(const Frustum& frustum, const vec3& point)
    {
        for (const vec4& plane : frustum.Planes)
        {
            if (glm::dot(vec3(plane), point) + plane.w < 0.0f)
            {
                return false;
            }
        }
        return true;
    }

    AABB BoxAt(const vec3& center, f32 halfExtent = 0.5f)
    {
        return AABB{.Min = center - vec3(halfExtent), .Max = center + vec3(halfExtent)};
    }

    // The engine's real camera: a Y-flipped Vulkan perspective looking down -Z
    // from the origin. The test pins extraction against this, not a bare
    // glm::perspective, so a Y-flip or ZO transcription slip surfaces here.
    Camera MakeTestCamera()
    {
        Camera camera;
        camera.SetPerspective(glm::radians(60.0f), 16.0f / 9.0f, 1.0f, 100.0f);
        camera.SetView(vec3(0.0f), vec3(0.0f, 0.0f, -1.0f), vec3(0.0f, 1.0f, 0.0f));
        return camera;
    }
}

TEST_CASE("FromViewProjection yields inward normals against the Y-flipped camera")
{
    const Camera camera = MakeTestCamera();
    const Frustum frustum = Frustum::FromViewProjection(camera.ViewProjection());

    // A point dead-center in the view (on the -Z axis, between near and far) is
    // inside every plane — the camera-frustum interior.
    CHECK(PointInside(frustum, vec3(0.0f, 0.0f, -10.0f)));

    // Points offset toward each screen edge but well within the FOV stay inside.
    // The up/down pair is the case a centroid-only check can miss if the
    // Y-flipped top/bottom planes were mis-oriented.
    CHECK(PointInside(frustum, vec3(0.0f, 1.0f, -10.0f)));
    CHECK(PointInside(frustum, vec3(0.0f, -1.0f, -10.0f)));
    CHECK(PointInside(frustum, vec3(1.0f, 0.0f, -10.0f)));
    CHECK(PointInside(frustum, vec3(-1.0f, 0.0f, -10.0f)));

    // A point far outside the FOV (way off to the side) is rejected.
    CHECK_FALSE(PointInside(frustum, vec3(100.0f, 0.0f, -10.0f)));
    // A point behind the camera (+Z) is rejected by the near plane.
    CHECK_FALSE(PointInside(frustum, vec3(0.0f, 0.0f, 10.0f)));
}

TEST_CASE("The ZO near plane is the Vulkan/GL discriminator")
{
    const Camera camera = MakeTestCamera();
    const mat4 viewProj = camera.ViewProjection();
    const Frustum frustum = Frustum::FromViewProjection(viewProj);

    // A point just in front of the near plane (near=1): closer than near but not
    // against the camera. Its clip.z < 0 (outside the Vulkan z=0 near) while
    // clip.z + clip.w > 0 (it would pass the GL row4+row3 near). The divergence
    // slab — between the GL near (ndc.z=-1) and the ZO near (ndc.z=0) — is exactly
    // where the ZO and GL forms disagree.
    const vec3 inSlab(0.0f, 0.0f, -0.95f);
    const vec4 clip = viewProj * vec4(inSlab, 1.0f);
    REQUIRE(clip.z < 0.0f);
    REQUIRE(clip.z + clip.w > 0.0f);

    // The ZO extraction culls it (the near plane is the third clip row alone).
    // A GL-form near plane would wrongly keep it — this is the regression guard.
    CHECK_FALSE(PointInside(frustum, inSlab));

    // A point just past the near plane (inside the ZO frustum) is kept.
    CHECK(PointInside(frustum, vec3(0.0f, 0.0f, -2.0f)));
}

TEST_CASE("Intersects: box wholly inside, wholly outside each plane, straddling")
{
    const Camera camera = MakeTestCamera();
    const Frustum frustum = Frustum::FromViewProjection(camera.ViewProjection());

    // Wholly inside.
    CHECK(Intersects(frustum, BoxAt(vec3(0.0f, 0.0f, -10.0f))));

    // Wholly outside each plane in turn: far left/right/up/down, beyond far,
    // behind near.
    CHECK_FALSE(Intersects(frustum, BoxAt(vec3(-100.0f, 0.0f, -10.0f))));
    CHECK_FALSE(Intersects(frustum, BoxAt(vec3(100.0f, 0.0f, -10.0f))));
    CHECK_FALSE(Intersects(frustum, BoxAt(vec3(0.0f, 100.0f, -10.0f))));
    CHECK_FALSE(Intersects(frustum, BoxAt(vec3(0.0f, -100.0f, -10.0f))));
    CHECK_FALSE(Intersects(frustum, BoxAt(vec3(0.0f, 0.0f, -1000.0f)))); // past far
    CHECK_FALSE(Intersects(frustum, BoxAt(vec3(0.0f, 0.0f, 10.0f))));    // behind near

    // Straddling the near plane (centered on z = -1, the near distance) → true:
    // a box on a plane is not culled.
    CHECK(Intersects(frustum, BoxAt(vec3(0.0f, 0.0f, -1.0f), 0.25f)));
}

TEST_CASE("Intersects is conservative — no false cull near the frustum boundary")
{
    const Camera camera = MakeTestCamera();
    const Frustum frustum = Frustum::FromViewProjection(camera.ViewProjection());

    // Sweep boxes across a slab of the view volume. Any box that contains a point
    // genuinely inside the frustum must not be culled — a false negative would be
    // a dropped visible mesh.
    for (f32 x = -30.0f; x <= 30.0f; x += 2.0f)
    {
        for (f32 y = -30.0f; y <= 30.0f; y += 2.0f)
        {
            for (f32 z = -50.0f; z <= -2.0f; z += 3.0f)
            {
                const AABB box = BoxAt(vec3(x, y, z), 1.0f);

                bool anyCornerInside = false;
                for (const vec3& corner : box.Corners())
                {
                    if (PointInside(frustum, corner))
                    {
                        anyCornerInside = true;
                        break;
                    }
                }

                if (anyCornerInside)
                {
                    CHECK(Intersects(frustum, box));
                }
            }
        }
    }
}

TEST_CASE("Ortho extraction yields axis-aligned planes bounding the ortho box")
{
    // A cascade's shape: an orthographic projection (ZO clip, the engine's glm
    // config), identity view. The frustum planes bound the ortho box directly.
    const mat4 ortho = glm::ortho(-10.0f, 10.0f, -10.0f, 10.0f, 1.0f, 50.0f);
    const Frustum frustum = Frustum::FromViewProjection(ortho);

    // A box at the box edge intersects.
    CHECK(Intersects(frustum, BoxAt(vec3(9.5f, 0.0f, -10.0f), 0.25f)));
    CHECK(Intersects(frustum, BoxAt(vec3(0.0f, 9.5f, -10.0f), 0.25f)));
    CHECK(Intersects(frustum, BoxAt(vec3(0.0f, 0.0f, -10.0f), 0.25f)));

    // A box just past each face does not.
    CHECK_FALSE(Intersects(frustum, BoxAt(vec3(11.0f, 0.0f, -10.0f), 0.25f)));
    CHECK_FALSE(Intersects(frustum, BoxAt(vec3(0.0f, 11.0f, -10.0f), 0.25f)));
    CHECK_FALSE(Intersects(frustum, BoxAt(vec3(0.0f, 0.0f, -0.5f), 0.25f)));  // before near (z=-1)
    CHECK_FALSE(Intersects(frustum, BoxAt(vec3(0.0f, 0.0f, -51.0f), 0.25f))); // past far (z=-50)
}
