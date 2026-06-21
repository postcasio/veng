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
    CameraView MakeTestCamera(f32 near = 0.1f, f32 far = 100.0f)
    {
        CameraView camera;
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
    const CameraView camera = MakeTestCamera(0.1f, 100.0f);
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
    const CameraView camera = MakeTestCamera();

    const CascadeData zero = ComputeCascades(camera, vec3(0.0f, -1.0f, 0.0f), AABB::Empty(),
                                             CascadeSettings{.Count = 0});
    CHECK(zero.Count == 1);

    const CascadeData over = ComputeCascades(camera, vec3(0.0f, -1.0f, 0.0f), AABB::Empty(),
                                             CascadeSettings{.Count = 16});
    CHECK(over.Count == MaxCascades);
}

TEST_CASE("ComputeCascades: Lambda endpoints reproduce uniform and logarithmic splits")
{
    const CameraView camera = MakeTestCamera(0.1f, 100.0f);
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
    const CameraView camera = MakeTestCamera(0.1f, 100.0f);
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
    const CameraView camera = MakeTestCamera();
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
    CameraView a;
    a.SetPerspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
    a.SetView(vec3(0.0f, 2.0f, 10.0f), vec3(0.0f, 2.0f, 0.0f), vec3(0.0f, 1.0f, 0.0f));

    CameraView b;
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

    CameraView a;
    a.SetPerspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
    a.SetView(vec3(0.0f, 2.0f, 10.0f), vec3(0.0f, 0.0f, 0.0f), vec3(0.0f, 1.0f, 0.0f));

    // Translate the camera by a tiny amount — well under a texel for the near
    // cascade — and re-fit. The snapped light-space box (its XY scale) should be
    // identical within a tight epsilon.
    CameraView b;
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

TEST_CASE("ComputeCascades: scene bound clamps the split range to the scene")
{
    // A camera whose far plane sits far past the scene (an editor fly camera defaults
    // to a 1000-unit far). Without the bound clamp the whole scene collapses into
    // cascade 0; the bound pulls the far split in to the scene's view-depth extent.
    const CameraView camera = MakeTestCamera(0.1f, 1000.0f);
    const vec3 lightDir(0.3f, -1.0f, 0.2f);
    const CascadeSettings settings{};

    const AABB sceneBounds{.Min = vec3(-15.0f, -2.0f, -15.0f), .Max = vec3(15.0f, 8.0f, 15.0f)};
    const CascadeData fitted = ComputeCascades(camera, lightDir, sceneBounds, settings);
    const CascadeData unfitted = ComputeCascades(camera, lightDir, AABB::Empty(), settings);

    // An empty bound keeps the camera's own far; a real bound fits the far split in.
    CHECK(unfitted.SplitFar[unfitted.Count - 1] == camera.GetFar());
    CHECK(fitted.SplitFar[fitted.Count - 1] < unfitted.SplitFar[unfitted.Count - 1]);
    // The scene lies within a few tens of view-units, nowhere near the 1000 far plane.
    CHECK(fitted.SplitFar[fitted.Count - 1] < 60.0f);
    // The fitted splits stay strictly increasing inside the tightened range.
    for (u32 i = 1; i < fitted.Count; ++i)
    {
        CHECK(fitted.SplitFar[i] > fitted.SplitFar[i - 1]);
    }
}

TEST_CASE("ComputeCascades: a bound fully behind the camera leaves the range untouched")
{
    const CameraView camera = MakeTestCamera(0.1f, 100.0f);
    const vec3 lightDir(0.3f, -1.0f, 0.2f);
    const CascadeSettings settings{};

    // The camera sits at z=10 looking toward the origin (−Z); a bound entirely behind
    // it (large +Z) has no view-depth extent in front, so the clamp degrades to a
    // no-op — the camera's own range — rather than a degenerate near==far.
    const AABB behind{.Min = vec3(-5.0f, -5.0f, 30.0f), .Max = vec3(5.0f, 5.0f, 40.0f)};
    const CascadeData data = ComputeCascades(camera, lightDir, behind, settings);
    const CascadeData empty = ComputeCascades(camera, lightDir, AABB::Empty(), settings);

    CHECK(data.SplitFar[data.Count - 1] == camera.GetFar());
    for (u32 i = 0; i < data.Count; ++i)
    {
        CHECK(data.SplitFar[i] == doctest::Approx(empty.SplitFar[i]).epsilon(1e-5));
    }
}

TEST_CASE("ComputeCascades: scene bound extends cascade 0's near plane toward the light")
{
    const CameraView camera = MakeTestCamera(0.1f, 100.0f);
    const vec3 lightDir(0.3f, -1.0f, 0.2f);
    const CascadeSettings settings{};

    // A bound tall along the light axis (a tower of casters above the ground) extends
    // cascade 0's near plane toward the light, so a caster above the frustum slice
    // still projects within the cascade's depth range rather than being clipped at the
    // near plane (ndc.z < 0).
    const AABB tall{.Min = vec3(-20.0f, -1.0f, -20.0f), .Max = vec3(20.0f, 30.0f, 20.0f)};
    const CascadeData data = ComputeCascades(camera, lightDir, tall, settings);

    const vec3 highCaster(0.0f, 25.0f, 0.0f);
    const vec4 clip = data.ViewProj[0] * vec4(highCaster, 1.0f);
    const vec3 ndc = vec3(clip) / clip.w;
    CHECK(ndc.z >= -1e-3f);
    CHECK(ndc.z <= 1.0f + 1e-3f);
}

TEST_CASE("ComputeCascades: split range fits the frustum-visible slab, not the whole bound")
{
    // A camera at the origin looking down −Z; a wide bound offset hard to the +X side so
    // its nearest corner lies OUTSIDE the narrow near frustum. The visible part of the
    // bound only begins deeper, where the frustum has widened to reach X=20 (depth ~35
    // at a 60° square FOV) — so fitting to the frustum∩bound intersection places cascade
    // 0 far deeper than fitting to the whole bound's view-depth extent, whose nearest
    // corner sits at depth ~1, would.
    CameraView camera;
    camera.SetPerspective(glm::radians(60.0f), 1.0f, 0.1f, 1000.0f);
    camera.SetView(vec3(0.0f, 0.0f, 0.0f), vec3(0.0f, 0.0f, -1.0f), vec3(0.0f, 1.0f, 0.0f));
    const vec3 lightDir(0.3f, -1.0f, 0.2f);
    const CascadeSettings settings{};

    const AABB offset{.Min = vec3(20.0f, -5.0f, -100.0f), .Max = vec3(60.0f, 5.0f, -1.0f)};
    const CascadeData data = ComputeCascades(camera, lightDir, offset, settings);

    // Fitting the whole bound's view-depth extent (nearest corner at depth ~1) would put
    // cascade 0's far split near ~6.5; fitting only the frustum-visible slab (near ~35)
    // pushes it well past 25.
    CHECK(data.SplitFar[0] > 25.0f);
}
