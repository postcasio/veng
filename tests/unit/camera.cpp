// Camera math: the view/projection construction the scene renderer draws
// through. Pure CPU — no Context, no Vulkan symbol touched. Pins the engine's
// clip conventions (the Vulkan Y-flip), the near/far the camera carries
// alongside the projection, and position recovery from the view's inverse.

#include <doctest/doctest.h>

#include <glm/gtc/matrix_transform.hpp>

#include <Veng/Scene/Camera.h>

using namespace Veng;

namespace
{
    bool VecApprox(const vec3& a, const vec3& b, f32 eps = 1e-4f)
    {
        return glm::all(glm::lessThan(glm::abs(a - b), vec3(eps)));
    }

    bool MatrixApprox(const mat4& a, const mat4& b, f32 eps = 1e-4f)
    {
        for (int c = 0; c < 4; ++c)
        {
            for (int r = 0; r < 4; ++r)
            {
                if (std::abs(a[c][r] - b[c][r]) > eps)
                {
                    return false;
                }
            }
        }
        return true;
    }
}

TEST_CASE("SetPerspective flips Y for Vulkan clip space and carries near/far")
{
    CameraView camera;
    camera.SetPerspective(glm::radians(60.0f), 16.0f / 9.0f, 0.25f, 250.0f);

    // glm::perspective yields a positive [1][1]; the engine negates it so clip
    // space has Y pointing down. The flip is the whole point — assert its sign.
    CHECK(camera.Projection()[1][1] < 0.0f);

    // The camera carries the range SetPerspective was given (reconstruction from
    // the matrix is fiddly under the flip, so it is stored, not derived).
    CHECK(camera.GetNear() == doctest::Approx(0.25f));
    CHECK(camera.GetFar() == doctest::Approx(250.0f));
}

TEST_CASE("SetOrthographic flips Y for Vulkan clip space and carries near/far")
{
    CameraView camera;
    camera.SetOrthographic(4.0f, 3.0f, 0.5f, 50.0f);

    // Same Y-flip as the perspective path: glm::ortho yields a positive [1][1],
    // the engine negates it so clip space has Y pointing down.
    CHECK(camera.Projection()[1][1] < 0.0f);

    CHECK(camera.GetNear() == doctest::Approx(0.5f));
    CHECK(camera.GetFar() == doctest::Approx(50.0f));
}

TEST_CASE("Orthographic projection has no perspective foreshortening")
{
    CameraView camera;
    camera.SetOrthographic(2.0f, 2.0f, 0.1f, 100.0f);
    camera.SetView(vec3{0.0f, 0.0f, 5.0f}, vec3{0.0f}, vec3{0.0f, 1.0f, 0.0f});

    // A unit-width span projects to the same clip-X extent whether it sits near
    // or far from the camera — the defining property of a parallel projection.
    const auto clipX = [&](vec3 world)
    {
        const vec4 clip = camera.ViewProjection() * vec4{world, 1.0f};
        return clip.x / clip.w;
    };

    const f32 nearSpan = clipX(vec3{1.0f, 0.0f, 2.0f}) - clipX(vec3{-1.0f, 0.0f, 2.0f});
    const f32 farSpan = clipX(vec3{1.0f, 0.0f, -3.0f}) - clipX(vec3{-1.0f, 0.0f, -3.0f});
    CHECK(nearSpan == doctest::Approx(farSpan));
}

TEST_CASE("Y-flip maps a world-up point to negative clip Y")
{
    CameraView camera;
    camera.SetPerspective(glm::radians(90.0f), 1.0f, 0.1f, 100.0f);
    camera.SetView(vec3{0.0f}, vec3{0.0f, 0.0f, -1.0f}, vec3{0.0f, 1.0f, 0.0f});

    // A point above the view centre and in front of the camera. Under the
    // Vulkan Y-flip, up projects to negative clip Y (it would be positive
    // without the flip — this is what the flip exists to correct).
    const vec4 up = camera.ViewProjection() * vec4{0.0f, 1.0f, -2.0f, 1.0f};
    CHECK(up.w > 0.0f);
    CHECK(up.y / up.w < 0.0f);

    // A point on the view axis projects to clip-Y zero.
    const vec4 centre = camera.ViewProjection() * vec4{0.0f, 0.0f, -2.0f, 1.0f};
    CHECK(centre.y / centre.w == doctest::Approx(0.0f));
}

TEST_CASE("SetView places the camera; GetPosition recovers the eye")
{
    CameraView camera;
    const vec3 eye{3.0f, 4.0f, 5.0f};
    camera.SetView(eye, vec3{0.0f}, vec3{0.0f, 1.0f, 0.0f});

    CHECK(VecApprox(camera.GetPosition(), eye));
}

TEST_CASE("SetViewFromWorld is the world matrix's inverse and recovers position")
{
    const vec3 eye{-2.0f, 7.0f, 1.5f};
    const mat4 world = glm::translate(mat4{1.0f}, eye) *
                       glm::rotate(mat4{1.0f}, glm::radians(35.0f), vec3{0.0f, 1.0f, 0.0f});

    CameraView camera;
    camera.SetViewFromWorld(world);

    CHECK(MatrixApprox(camera.View(), glm::inverse(world)));
    // The position is the world's translation column regardless of rotation.
    CHECK(VecApprox(camera.GetPosition(), eye));
}

TEST_CASE("ViewProjection composes Projection * View")
{
    CameraView camera;
    camera.SetPerspective(glm::radians(50.0f), 4.0f / 3.0f, 0.1f, 80.0f);
    camera.SetView(vec3{1.0f, 2.0f, 6.0f}, vec3{0.0f}, vec3{0.0f, 1.0f, 0.0f});

    CHECK(MatrixApprox(camera.ViewProjection(), camera.Projection() * camera.View()));
}

TEST_CASE("MakeCameraView composes a Camera component, aspect, and world matrix")
{
    Camera component;
    component.FovY = glm::radians(70.0f);
    component.Near = 0.2f;
    component.Far = 120.0f;

    const f32 aspect = 16.0f / 9.0f;
    const vec3 eye{0.0f, 3.0f, 9.0f};
    const mat4 world = glm::translate(mat4{1.0f}, eye);

    const CameraView made = MakeCameraView(component, aspect, world);

    CameraView expected;
    expected.SetPerspective(component.FovY, aspect, component.Near, component.Far);
    expected.SetViewFromWorld(world);

    CHECK(MatrixApprox(made.Projection(), expected.Projection()));
    CHECK(MatrixApprox(made.View(), expected.View()));
    CHECK(VecApprox(made.GetPosition(), eye));
    CHECK(made.GetNear() == doctest::Approx(component.Near));
    CHECK(made.GetFar() == doctest::Approx(component.Far));
}

TEST_CASE("MakeCameraView resolves an Orthographic component through SetOrthographic")
{
    // OrthoHeight is the full vertical extent; the half-width follows the aspect.
    Camera component;
    component.Projection = CameraProjection::Orthographic;
    component.OrthoHeight = 9.0f;
    component.Near = 0.5f;
    component.Far = 60.0f;

    const f32 aspect = 16.0f / 9.0f;
    const vec3 eye{2.0f, 1.0f, 12.0f};
    const mat4 world = glm::translate(mat4{1.0f}, eye);

    const CameraView made = MakeCameraView(component, aspect, world);

    CameraView expected;
    expected.SetOrthographic(4.5f * aspect, 4.5f, component.Near, component.Far);
    expected.SetViewFromWorld(world);

    CHECK(MatrixApprox(made.Projection(), expected.Projection()));
    CHECK(MatrixApprox(made.View(), expected.View()));
    CHECK(VecApprox(made.GetPosition(), eye));
    CHECK(made.GetNear() == doctest::Approx(component.Near));
    CHECK(made.GetFar() == doctest::Approx(component.Far));
}

TEST_CASE("ProjectToScreen maps world points to top-left-origin pixels and rejects behind-camera")
{
    // Looking down -Z from the origin: world center projects to the extent's center, a point
    // above center lands in the upper half (top-left origin), and a point behind returns nullopt.
    CameraView camera;
    camera.SetPerspective(glm::radians(90.0f), 1.0f, 0.1f, 100.0f);
    camera.SetView(vec3(0.0f), vec3(0.0f, 0.0f, -1.0f), vec3(0.0f, 1.0f, 0.0f));

    const vec2 extent(200.0f, 200.0f);

    const optional<vec2> center = ProjectToScreen(camera, vec3(0.0f, 0.0f, -10.0f), extent);
    REQUIRE(center.has_value());
    CHECK(center->x == doctest::Approx(100.0f));
    CHECK(center->y == doctest::Approx(100.0f));

    // At fovY 90° and distance 10, a point 10 up sits exactly on the top edge (y = 0).
    const optional<vec2> above = ProjectToScreen(camera, vec3(0.0f, 10.0f, -10.0f), extent);
    REQUIRE(above.has_value());
    CHECK(above->x == doctest::Approx(100.0f));
    CHECK(above->y == doctest::Approx(0.0f));

    CHECK(!ProjectToScreen(camera, vec3(0.0f, 0.0f, 10.0f), extent).has_value());
}

TEST_CASE("far-plane view-direction reconstruction survives extreme Far/Near ratios")
{
    // The sky, TAA, and volume-field shaders reconstruct a background pixel's view ray as
    // normalize(InvViewProj·(ndc, 1, 1).xyz − camera·w) — the homogeneous form. The divided
    // form (xyz / w − camera) fails at extreme depth ranges: once Far/Near exceeds ~2^24 the
    // projection's [2][2] rounds to −1 exactly, the far-plane w collapses to zero in f32, and
    // the division sends every background direction to NaN. Pins both facts: the collapse is
    // real at the extreme ratio, and the homogeneous form stays exact through it.
    const vec3 eye{0.0f, 1.5f, 4.0f};
    const vec3 target{-1.0f, 1.5f, 4.0f};
    const vec2 ndc{0.30f, -0.20f};
    const f32 far = 20000.0f;

    // The f64 reference direction, from unrounded double-precision matrices end to end.
    const auto reference = [&](f64 near) -> glm::dvec3
    {
        glm::dmat4 proj = glm::perspective(glm::radians(60.0), 16.0 / 9.0, near, f64{far});
        proj[1][1] *= -1.0;
        const glm::dmat4 view =
            glm::lookAt(glm::dvec3(eye), glm::dvec3(target), glm::dvec3(0.0, 1.0, 0.0));
        const glm::dvec4 worldH = glm::inverse(proj * view) * glm::dvec4(ndc, 1.0, 1.0);
        return glm::normalize(glm::dvec3(worldH) - glm::dvec3(eye) * worldH.w);
    };

    // The f32 pipeline exactly as the renderer and shaders evaluate it: a CameraView
    // projection, an f32 matrix inverse, and the shader's reconstruction expressions.
    const auto reconstruct = [&](f32 ratio)
    {
        CameraView camera;
        camera.SetPerspective(glm::radians(60.0f), 16.0f / 9.0f, far / ratio, far);
        camera.SetView(eye, target, vec3(0.0f, 1.0f, 0.0f));
        const mat4 inv = glm::inverse(camera.ViewProjection());
        return vec4{inv * vec4(ndc, 1.0f, 1.0f)};
    };
    const auto degreesOff = [&](vec3 dir, glm::dvec3 truth)
    { return glm::degrees(std::acos(glm::clamp(glm::dot(glm::dvec3(dir), truth), -1.0, 1.0))); };

    // Ordinary ratio: both forms agree with the reference.
    {
        const f32 ratio = 1.0e4f;
        const vec4 worldH = reconstruct(ratio);
        const glm::dvec3 truth = reference(f64{far} / f64{ratio});
        const vec3 divided = glm::normalize(vec3(worldH) / worldH.w - eye);
        const vec3 homogeneous = glm::normalize(vec3(worldH) - eye * worldH.w);
        CHECK(degreesOff(divided, truth) < 0.01);
        CHECK(degreesOff(homogeneous, truth) < 0.01);
    }

    // Extreme ratio: the far-plane w collapses to zero, so the divided form is unusable —
    // only the homogeneous form reconstructs the direction.
    {
        const f32 ratio = 6.7e7f;
        const vec4 worldH = reconstruct(ratio);
        const glm::dvec3 truth = reference(f64{far} / f64{ratio});
        CHECK(worldH.w == 0.0f);
        const vec3 homogeneous = glm::normalize(vec3(worldH) - eye * worldH.w);
        CHECK(degreesOff(homogeneous, truth) < 0.01);
    }
}
