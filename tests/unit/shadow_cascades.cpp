// Cascade-math unit cases: pure CPU math, no Context, no Vulkan. ComputeCascades
// is a glm-only function of a Camera, a light direction, and a scene bound, so
// these run with no ICD.

#include <doctest/doctest.h>

#include <array>
#include <cmath>

#include <glm/gtc/matrix_transform.hpp>

#include <Veng/Math/AABB.h>
#include <Veng/Renderer/ShadowCascades.h>
#include <Veng/Scene/Camera.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    Camera MakeTestCamera(f32 near = 0.1f, f32 far = 100.0f)
    {
        Camera camera;
        camera.SetPerspective(glm::radians(60.0f), 16.0f / 9.0f, near, far);
        camera.SetView(vec3(0.0f, 2.0f, 10.0f), vec3(0.0f, 0.0f, 0.0f), vec3(0.0f, 1.0f, 0.0f));
        return camera;
    }

    bool IsFinite(const mat4& m)
    {
        for (int c = 0; c < 4; ++c)
        {
            for (int r = 0; r < 4; ++r)
            {
                if (!std::isfinite(m[c][r]))
                {
                    return false;
                }
            }
        }
        return true;
    }
}

TEST_CASE("ComputeCascades: SplitFar is strictly increasing and ends at far")
{
    const Camera camera = MakeTestCamera(0.1f, 100.0f);
    const CascadeData data =
        ComputeCascades(camera, vec3(0.3f, -1.0f, 0.2f), AABB::Empty(), CascadeSettings{});

    CHECK(data.Count == 4);
    CHECK(data.SplitFar[0] > camera.GetNear());
    for (u32 i = 1; i < data.Count; ++i)
    {
        CHECK(data.SplitFar[i] > data.SplitFar[i - 1]);
    }
    // The final split is the far plane, exactly.
    CHECK(data.SplitFar[data.Count - 1] == camera.GetFar());
}

TEST_CASE("ComputeCascades: Count clamps to [1, MaxCascades]")
{
    const Camera camera = MakeTestCamera();

    const CascadeData zero = ComputeCascades(camera, vec3(0.0f, -1.0f, 0.0f), AABB::Empty(),
                                             CascadeSettings{.Count = 0});
    CHECK(zero.Count == 1);

    const CascadeData over = ComputeCascades(camera, vec3(0.0f, -1.0f, 0.0f), AABB::Empty(),
                                             CascadeSettings{.Count = 16});
    CHECK(over.Count == MaxCascades);
}

TEST_CASE("ComputeCascades: Lambda endpoints reproduce uniform and logarithmic splits")
{
    const Camera camera = MakeTestCamera(0.1f, 100.0f);
    const f32 near = camera.GetNear();
    const f32 far = camera.GetFar();
    const u32 count = 4;

    const CascadeData uniform = ComputeCascades(camera, vec3(0.0f, -1.0f, 0.0f), AABB::Empty(),
                                                CascadeSettings{.Count = count, .Lambda = 0.0f});
    const CascadeData logarithmic =
        ComputeCascades(camera, vec3(0.0f, -1.0f, 0.0f), AABB::Empty(),
                        CascadeSettings{.Count = count, .Lambda = 1.0f});

    for (u32 i = 1; i <= count; ++i)
    {
        const f32 fraction = static_cast<f32>(i) / static_cast<f32>(count);
        const f32 expectedUniform = near + (far - near) * fraction;
        const f32 expectedLog = near * std::pow(far / near, fraction);

        // SplitFar[i-1] is the cascade's far = d[i]; the last is pinned to far.
        CHECK(uniform.SplitFar[i - 1] == doctest::Approx(expectedUniform).epsilon(0.001));
        CHECK(logarithmic.SplitFar[i - 1] == doctest::Approx(expectedLog).epsilon(0.001));
    }
}

TEST_CASE("ComputeCascades: interior slice points project inside their cascade")
{
    const Camera camera = MakeTestCamera(0.1f, 100.0f);
    const f32 near = camera.GetNear();
    const f32 far = camera.GetFar();
    const CascadeData data =
        ComputeCascades(camera, vec3(0.3f, -1.0f, 0.2f), AABB::Empty(), CascadeSettings{});

    const mat4 invViewProj = glm::inverse(camera.ViewProjection());

    auto worldPointAtViewDepth = [&](f32 viewDepth)
    {
        // Interpolate a center-of-screen world point between the near and far
        // frustum centers by the view-depth fraction (linear in view depth).
        const f32 fraction = (viewDepth - near) / (far - near);
        const vec4 nearH = invViewProj * vec4(0.0f, 0.0f, 0.0f, 1.0f);
        const vec4 farH = invViewProj * vec4(0.0f, 0.0f, 1.0f, 1.0f);
        const vec3 nearPoint = vec3(nearH) / nearH.w;
        const vec3 farPoint = vec3(farH) / farH.w;
        return nearPoint + (farPoint - nearPoint) * fraction;
    };

    auto inClip = [](const vec4& clip)
    {
        const vec3 ndc = vec3(clip) / clip.w;
        const f32 eps = 1e-3f;
        return ndc.x >= -1.0f - eps && ndc.x <= 1.0f + eps && ndc.y >= -1.0f - eps &&
               ndc.y <= 1.0f + eps && ndc.z >= -eps && ndc.z <= 1.0f + eps;
    };

    // An interior point of cascade 0 (~50% through its depth range).
    const f32 cascade0Mid = near + (data.SplitFar[0] - near) * 0.5f;
    const vec3 p0 = worldPointAtViewDepth(cascade0Mid);
    CHECK(inClip(data.ViewProj[0] * vec4(p0, 1.0f)));

    // An interior point of the last cascade (~95% depth — not the far corner,
    // which guard-band/snap rounding can push past z = 1).
    const u32 last = data.Count - 1;
    const f32 lastStart = data.SplitFar[last - 1];
    const f32 lastMid = lastStart + (data.SplitFar[last] - lastStart) * 0.5f;
    const vec3 pLast = worldPointAtViewDepth(lastMid);
    CHECK(inClip(data.ViewProj[last] * vec4(pLast, 1.0f)));
}

TEST_CASE("ComputeCascades: straight-down light yields finite matrices")
{
    const Camera camera = MakeTestCamera();
    const CascadeData data =
        ComputeCascades(camera, vec3(0.0f, -1.0f, 0.0f), AABB::Empty(), CascadeSettings{});

    for (u32 k = 0; k < data.Count; ++k)
    {
        CHECK(IsFinite(data.ViewProj[k]));
    }
}

TEST_CASE("ComputeCascades: cascade extent is rotation-invariant")
{
    // Two cameras at the same position, different yaw. The bounding-sphere fit
    // makes each cascade's extent (sphere diameter) independent of orientation.
    Camera a;
    a.SetPerspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
    a.SetView(vec3(0.0f, 2.0f, 10.0f), vec3(0.0f, 2.0f, 0.0f), vec3(0.0f, 1.0f, 0.0f));

    Camera b;
    b.SetPerspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
    b.SetView(vec3(0.0f, 2.0f, 10.0f), vec3(5.0f, 2.0f, 5.0f), vec3(0.0f, 1.0f, 0.0f));

    const vec3 lightDir(0.3f, -1.0f, 0.2f);
    const CascadeData da = ComputeCascades(a, lightDir, AABB::Empty(), CascadeSettings{});
    const CascadeData db = ComputeCascades(b, lightDir, AABB::Empty(), CascadeSettings{});

    // Recover each cascade's XY ortho extent from the projection scale: in
    // glm::orthoZO, proj[0][0] = 2/(right-left), so the width is 2/proj[0][0].
    // The matrices include lightView, but the projection scale survives.
    for (u32 k = 0; k < da.Count; ++k)
    {
        // The ortho scale is the X/Y row lengths of the linear part.
        const f32 widthA = glm::length(vec3(da.ViewProj[k][0]));
        const f32 widthB = glm::length(vec3(db.ViewProj[k][0]));
        CHECK(widthA == doctest::Approx(widthB).epsilon(0.01));
    }
}

TEST_CASE("ComputeCascades: sub-texel camera translation yields a stable snapped box")
{
    const vec3 lightDir(0.3f, -1.0f, 0.2f);
    const CascadeSettings settings{.Count = 4, .Lambda = 0.85f, .Resolution = 1024};

    Camera a;
    a.SetPerspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
    a.SetView(vec3(0.0f, 2.0f, 10.0f), vec3(0.0f, 0.0f, 0.0f), vec3(0.0f, 1.0f, 0.0f));

    // Translate the camera by a tiny amount — well under a texel for the near
    // cascade — and re-fit. The snapped light-space box (its XY scale) should be
    // identical within a tight epsilon.
    Camera b;
    b.SetPerspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
    b.SetView(vec3(0.0001f, 2.0f, 10.0001f), vec3(0.0001f, 0.0f, 0.0001f), vec3(0.0f, 1.0f, 0.0f));

    const CascadeData da = ComputeCascades(a, lightDir, AABB::Empty(), settings);
    const CascadeData db = ComputeCascades(b, lightDir, AABB::Empty(), settings);

    // The full ViewProj for cascade 0 should match within a tight epsilon: the
    // snapped min is identical and the extent is sphere-stable.
    for (int c = 0; c < 4; ++c)
    {
        for (int r = 0; r < 4; ++r)
        {
            CHECK(da.ViewProj[0][c][r] == doctest::Approx(db.ViewProj[0][c][r]).epsilon(0.001));
        }
    }
}

TEST_CASE("ComputeCascades: scene bound extends the near plane, leaving XY untouched")
{
    const Camera camera = MakeTestCamera(0.1f, 100.0f);
    const vec3 lightDir(0.3f, -1.0f, 0.2f);
    const CascadeSettings settings{};

    // A scene bound tall along the light axis (a tower of casters above the
    // ground) extends the cascade near plane toward the light.
    const AABB sceneBounds{.Min = vec3(-20.0f, -1.0f, -20.0f), .Max = vec3(20.0f, 30.0f, 20.0f)};

    const CascadeData withBounds = ComputeCascades(camera, lightDir, sceneBounds, settings);
    const CascadeData noBounds = ComputeCascades(camera, lightDir, AABB::Empty(), settings);

    // XY is provably untouched: the linear X/Y projection scale (rows 0 and 1)
    // is identical with and without the scene bound — only the near plane moved.
    for (u32 k = 0; k < withBounds.Count; ++k)
    {
        for (int c = 0; c < 4; ++c)
        {
            CHECK(withBounds.ViewProj[k][c][0] ==
                  doctest::Approx(noBounds.ViewProj[k][c][0]).epsilon(1e-5));
            CHECK(withBounds.ViewProj[k][c][1] ==
                  doctest::Approx(noBounds.ViewProj[k][c][1]).epsilon(1e-5));
        }
    }

    // A caster high above the slice, inside the bound, projects within z ∈ [0,1]
    // of cascade 0 thanks to the near-plane extension.
    const vec3 highCaster(0.0f, 25.0f, 0.0f);
    const vec4 clip = withBounds.ViewProj[0] * vec4(highCaster, 1.0f);
    const vec3 ndc = vec3(clip) / clip.w;
    CHECK(ndc.z >= -1e-3f);
    CHECK(ndc.z <= 1.0f + 1e-3f);
}
