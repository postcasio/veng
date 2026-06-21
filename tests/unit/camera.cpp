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
