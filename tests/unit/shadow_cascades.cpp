// Cascade-math unit cases: pure CPU math, no Context, no Vulkan. ComputeCascades
// is a glm-only function of a Camera, a light direction, a scene bound and a snap
// frame, so these run with no ICD.

#include <doctest/doctest.h>

#include <array>
#include <cmath>
#include <limits>

#include <glm/gtc/matrix_transform.hpp>
#include <glm/gtx/component_wise.hpp>

#include <Veng/Math/AABB.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/ShadowCascades.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include "Renderer/SceneFloatingOrigin.h"

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

    constexpr f32 TestFovY = glm::radians(60.0f);
    constexpr f32 TestAspect = 16.0f / 9.0f;

    CameraView MakeCamera(const vec3 eye, const vec3 forward, const f32 near = 0.1f,
                          const f32 far = 100.0f)
    {
        CameraView camera;
        camera.SetPerspective(TestFovY, TestAspect, near, far);
        camera.SetView(eye, eye + forward, vec3(0.0f, 1.0f, 0.0f));
        return camera;
    }

    // A point's texel coordinate in cascade k: (ndc.xy · 0.5 + 0.5) · resolution.
    vec2 TexelCoord(const CascadeData& data, const u32 k, const vec3 point, const u32 resolution)
    {
        const vec4 clip = data.ViewProj[k] * vec4(point, 1.0f);
        return (vec2(clip) / clip.w * 0.5f + 0.5f) * static_cast<f32>(resolution);
    }

    // How far two texel coordinates are from differing by a whole number of texels, per axis.
    f32 SubTexelMismatch(const vec2 a, const vec2 b)
    {
        const vec2 delta = a - b;
        return glm::compMax(glm::abs(delta - glm::round(delta)));
    }

    // The eight world-space corners of the camera's frustum between two view depths, from the
    // test camera's own field of view.
    std::array<vec3, 8> SliceCorners(const CameraView& camera, const f32 nearDepth,
                                     const f32 farDepth)
    {
        const mat4 viewToWorld = glm::inverse(camera.View());
        std::array<vec3, 8> corners{};
        usize i = 0;
        for (const f32 depth : {nearDepth, farDepth})
        {
            const f32 halfHeight = depth * std::tan(TestFovY * 0.5f);
            const f32 halfWidth = halfHeight * TestAspect;
            for (const vec2 sign :
                 {vec2(-1.0f, -1.0f), vec2(1.0f, -1.0f), vec2(1.0f, 1.0f), vec2(-1.0f, 1.0f)})
            {
                corners[i++] =
                    vec3(viewToWorld * vec4(sign.x * halfWidth, sign.y * halfHeight, -depth, 1.0f));
            }
        }
        return corners;
    }

    // The fewest texels between any slice corner of any cascade and the edge of its tile.
    f32 SliceMarginTexels(const CascadeData& data, const CameraView& camera, const u32 resolution)
    {
        const f32 res = static_cast<f32>(resolution);
        f32 margin = std::numeric_limits<f32>::max();
        for (u32 k = 0; k < data.Count; ++k)
        {
            const f32 nearDepth = k == 0 ? camera.GetNear() : data.SplitFar[k - 1];
            for (const vec3& corner : SliceCorners(camera, nearDepth, data.SplitFar[k]))
            {
                const vec2 u = TexelCoord(data, k, corner, resolution);
                margin = std::min({margin, u.x, u.y, res - u.x, res - u.y});
            }
        }
        return margin;
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

TEST_CASE("ComputeCascades: every slice lands inside its tile with a texel to spare")
{
    // Stratified: three light directions (one straight down), two camera poses, and a fixed
    // origin and one 1e5 units out. The box is exactly Resolution texels of the reported pitch.
    const CascadeSettings settings{.Resolution = 1024};
    const std::array<vec3, 3> lights = {vec3(0.3f, -1.0f, 0.2f), vec3(0.0f, -1.0f, 0.0f),
                                        vec3(-0.9f, -0.25f, 0.4f)};
    const std::array<CameraView, 2> cameras = {
        MakeCamera(vec3(0.0f, 2.0f, 10.0f), vec3(0.0f, -0.2f, -1.0f)),
        MakeCamera(vec3(-3.7f, 0.4f, 1.9f), vec3(0.8f, 0.3f, 0.5f))};
    const std::array<dvec3, 2> origins = {dvec3(0.0), dvec3(1.0e5, -4.1e4, 7.3e4)};

    f32 margin = std::numeric_limits<f32>::max();
    f32 widthError = 0.0f;
    u32 fits = 0;
    for (const vec3& light : lights)
    {
        for (const CameraView& camera : cameras)
        {
            for (const dvec3& origin : origins)
            {
                const CascadeData data =
                    ComputeCascades(camera, light, AABB::Empty(), settings, {.Origin = origin});
                margin = std::min(margin, SliceMarginTexels(data, camera, settings.Resolution));
                for (u32 k = 0; k < data.Count; ++k)
                {
                    // orthoZO: |X row| = 2 / width, row 0 being glm's [c][0] across columns.
                    const f32 width =
                        2.0f / glm::length(vec3(data.ViewProj[k][0][0], data.ViewProj[k][1][0],
                                                data.ViewProj[k][2][0]));
                    const f32 expected =
                        static_cast<f32>(settings.Resolution) * data.TexelWorldSize[k];
                    widthError = std::max(widthError, std::abs(width - expected) / expected);
                }
                ++fits;
            }
        }
    }
    CHECK(fits == 12);
    CHECK(margin >= 1.0f);
    CHECK(widthError < 1e-5f);
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

TEST_CASE("ComputeCascades: turning changes neither the texel size nor the grid")
{
    // The camera stays put and yaws 0°, 37° and 180°, then pitches 20°. An empty bound keeps the
    // splits fixed, so every pose measures the same slices.
    const vec3 eye(0.4f, 2.0f, 3.0f);
    const vec3 lightDir(0.3f, -1.0f, 0.2f);
    const CascadeSettings settings{.Resolution = 1024};
    const auto forward = [](const f32 yawDegrees, const f32 pitchDegrees)
    {
        const f32 yaw = glm::radians(yawDegrees);
        const f32 pitch = glm::radians(pitchDegrees);
        return vec3(std::sin(yaw) * std::cos(pitch), std::sin(pitch),
                    -std::cos(yaw) * std::cos(pitch));
    };
    const std::array<CameraView, 4> poses = {
        MakeCamera(eye, forward(0.0f, 0.0f)), MakeCamera(eye, forward(37.0f, 0.0f)),
        MakeCamera(eye, forward(180.0f, 0.0f)), MakeCamera(eye, forward(180.0f, 20.0f))};

    const CascadeData first = ComputeCascades(poses[0], lightDir, AABB::Empty(), settings);
    // A point beside the eye lies in cascade 0's tile however the camera turns.
    const vec3 point = eye + vec3(0.3f, -0.2f, 0.1f);
    const vec2 firstTexel = TexelCoord(first, 0, point, settings.Resolution);
    const f32 res = static_cast<f32>(settings.Resolution);
    REQUIRE(glm::all(glm::greaterThan(firstTexel, vec2(0.0f))));
    REQUIRE(glm::all(glm::lessThan(firstTexel, vec2(res))));

    u32 sizeMismatches = 0;
    u32 outsideTile = 0;
    f32 worstMismatch = 0.0f;
    for (usize i = 1; i < poses.size(); ++i)
    {
        const CascadeData data = ComputeCascades(poses[i], lightDir, AABB::Empty(), settings);
        for (u32 k = 0; k < data.Count; ++k)
        {
            sizeMismatches += data.TexelWorldSize[k] == first.TexelWorldSize[k] &&
                                      data.RadiusStep[k] == first.RadiusStep[k]
                                  ? 0u
                                  : 1u;
        }
        const vec2 texel = TexelCoord(data, 0, point, settings.Resolution);
        outsideTile += glm::all(glm::greaterThan(texel, vec2(0.0f))) &&
                               glm::all(glm::lessThan(texel, vec2(res)))
                           ? 0u
                           : 1u;
        worstMismatch = std::max(worstMismatch, SubTexelMismatch(texel, firstTexel));
    }
    CHECK(sizeMismatches == 0);
    CHECK(outsideTile == 0);
    CHECK(worstMismatch < 1e-3f);
}

TEST_CASE("ComputeCascades: a translation leaves the grid where it was")
{
    // A fixed-origin scene and an empty bound, so the splits do not move. The camera steps a
    // fraction of a texel, then several, along a diagonal; a point inside cascade 0's slice in
    // both poses keeps its sub-texel position, and the box moves by whole texels.
    const vec3 lightDir(0.3f, -1.0f, 0.2f);
    const CascadeSettings settings{.Resolution = 1024};
    const vec3 eye(0.2f, 2.0f, 3.0f);
    const vec3 forward = glm::normalize(vec3(0.1f, -0.3f, -1.0f));
    const CameraView start = MakeCamera(eye, forward);
    const CascadeData a = ComputeCascades(start, lightDir, AABB::Empty(), settings);
    const vec3 point = eye + forward * 1.5f;
    REQUIRE(1.5f < a.SplitFar[0]);
    const vec2 texelA = TexelCoord(a, 0, point, settings.Resolution);

    const vec3 diagonal = glm::normalize(vec3(1.0f, 1.0f, 1.0f));
    for (const f32 texels : {0.3f, 7.6f})
    {
        const CameraView moved =
            MakeCamera(eye + diagonal * (texels * a.TexelWorldSize[0]), forward);
        const CascadeData b = ComputeCascades(moved, lightDir, AABB::Empty(), settings);
        REQUIRE(b.TexelWorldSize[0] == a.TexelWorldSize[0]);
        const vec2 texelB = TexelCoord(b, 0, point, settings.Resolution);
        CHECK(SubTexelMismatch(texelA, texelB) < 1e-3f);
        if (texels > 1.0f)
        {
            // The box followed the camera, by whole texels.
            CHECK(glm::compMax(glm::abs(glm::round(texelA - texelB))) >= 1.0f);
        }
    }
}

TEST_CASE("ComputeCascades: a floating-origin shift moves nothing")
{
    // Frame B is frame A with every render-space position shifted by -d and the origin moved by
    // +d, so the same scene stands in the same place in the stable frame.
    const vec3 lightDir(0.3f, -1.0f, 0.2f);
    const CascadeSettings settings{.Resolution = 1024};
    const vec3 eye(0.2f, 2.0f, 3.0f);
    const vec3 forward = glm::normalize(vec3(0.1f, -0.3f, -1.0f));
    const dvec3 origin(1.0e5, -3.7e4, 6.2e4);
    const CascadeData a = ComputeCascades(MakeCamera(eye, forward), lightDir, AABB::Empty(),
                                          settings, {.Origin = origin});
    const vec3 point = eye + forward * 1.5f;

    for (const vec3 shift : {vec3(0.3f * a.TexelWorldSize[0]), vec3(12.3f, -4.5f, 7.8f)})
    {
        const CascadeData b =
            ComputeCascades(MakeCamera(eye - shift, forward), lightDir, AABB::Empty(), settings,
                            {.Origin = origin + dvec3(shift)});
        f32 worst = 0.0f;
        for (u32 k = 0; k < a.Count; ++k)
        {
            const vec2 texelA = TexelCoord(a, k, point, settings.Resolution);
            const vec2 texelB = TexelCoord(b, k, point - shift, settings.Resolution);
            worst = std::max(worst, glm::compMax(glm::abs(texelA - texelB)));
        }
        CHECK(worst < 1e-3f);
    }
}

TEST_CASE("ComputeCascades: texel lines lie on the stable frame's grid at any origin")
{
    // A point's sub-texel position is its stable-frame coordinate on the light's axes, in
    // pitches, whatever the origin's magnitude: the grid is fixed in that frame, not in the
    // render space the camera moves through.
    const vec3 lightDir(0.3f, -1.0f, 0.2f);
    const CascadeSettings settings{.Resolution = 1024};
    const vec3 eye(0.2f, 2.0f, 3.0f);
    const vec3 forward = glm::normalize(vec3(0.1f, -0.3f, -1.0f));
    const CameraView camera = MakeCamera(eye, forward);
    const vec3 point = eye + forward * 1.5f;
    const mat3 basis = CascadeLightBasis(lightDir);

    f32 worst = 0.0f;
    for (const dvec3 origin : {dvec3(0.0), dvec3(1234.567, -876.5, 345.25),
                               dvec3(1.0e5, -3.7e4, 6.2e4), dvec3(3.0e5, 1.1e5, -2.4e5)})
    {
        const CascadeData data =
            ComputeCascades(camera, lightDir, AABB::Empty(), settings, {.Origin = origin});
        const dvec3 stable = dvec3(point) + origin;
        for (u32 k = 0; k < data.Count; ++k)
        {
            const f64 pitch = data.TexelWorldSize[k];
            const vec2 texel = TexelCoord(data, k, point, settings.Resolution);
            for (glm::length_t axis = 0; axis < 2; ++axis)
            {
                const f64 along = (static_cast<f64>(basis[0][axis]) * stable.x) +
                                  (static_cast<f64>(basis[1][axis]) * stable.y) +
                                  (static_cast<f64>(basis[2][axis]) * stable.z);
                // The clip space's y points down, so texel y counts against the light's up axis.
                const f64 sign = axis == 0 ? 1.0 : -1.0;
                const f64 delta = static_cast<f64>(texel[axis]) - (sign * along / pitch);
                worst = std::max(worst, static_cast<f32>(std::abs(delta - std::round(delta))));
            }
        }
    }
    CHECK(worst < 1e-3f);
}

TEST_CASE("ComputeCascades: near cascades ride the viewer, far cascades rest in the stable frame")
{
    // Four cascades over fixed splits, and a reach between the first and second slices' near
    // depths. In frame B the viewer has moved by d and the world is re-centred on it: the camera
    // and a carried point stay put in render space, the origin moves by d, and a point at rest
    // moves by -d.
    const vec3 lightDir(0.3f, -1.0f, 0.2f);
    const CascadeSettings settings{.Count = 4, .Resolution = 1024};
    const vec3 eye(0.2f, 2.0f, 3.0f);
    const vec3 forward = glm::normalize(vec3(0.1f, -0.3f, -1.0f));
    const CameraView camera = MakeCamera(eye, forward);
    const dvec3 origin(1.0e5, -3.7e4, 6.2e4);

    const CascadeData plain = ComputeCascades(camera, lightDir, AABB::Empty(), settings);
    const f32 reach = 0.5f * (camera.GetNear() + plain.SplitFar[0]);
    const CascadeData a = ComputeCascades(camera, lightDir, AABB::Empty(), settings,
                                          {.Origin = origin, .ViewerFrameReach = reach});
    CHECK(a.ViewerFrameCount == 1);

    const vec3 carried = eye + forward * 1.2f + vec3(0.2f, 0.1f, 0.0f);
    const vec3 resting = eye + forward * 20.0f;
    for (const vec3 shift : {vec3(0.3f * a.TexelWorldSize[0]), vec3(21.0f, -9.4f, 28.7f)})
    {
        const CascadeData b =
            ComputeCascades(camera, lightDir, AABB::Empty(), settings,
                            {.Origin = origin + dvec3(shift), .ViewerFrameReach = reach});
        CHECK(b.ViewerFrameCount == 1);
        // The viewer-frame cascade holds still against the viewer, wholly.
        const vec2 carriedA = TexelCoord(a, 0, carried, settings.Resolution);
        const vec2 carriedB = TexelCoord(b, 0, carried, settings.Resolution);
        CHECK(glm::compMax(glm::abs(carriedA - carriedB)) < 1e-3f);
        // The stable-frame cascades hold still against the resting point; their boxes still
        // follow the viewer, by whole texels.
        f32 worst = 0.0f;
        for (u32 k = 1; k < a.Count; ++k)
        {
            worst = std::max(
                worst, SubTexelMismatch(TexelCoord(a, k, resting, settings.Resolution),
                                        TexelCoord(b, k, resting - shift, settings.Resolution)));
        }
        CHECK(worst < 1e-3f);
    }

    // A reach past every slice's near depth puts every cascade in the viewer's frame: the fit
    // collapsed onto carried geometry. A zero reach puts none there.
    const CascadeData all =
        ComputeCascades(camera, lightDir, AABB::Empty(), settings,
                        {.Origin = origin, .ViewerFrameReach = plain.SplitFar[plain.Count - 1]});
    CHECK(all.ViewerFrameCount == all.Count);
    const CascadeData shifted =
        ComputeCascades(camera, lightDir, AABB::Empty(), settings,
                        {.Origin = origin + dvec3(21.0f, -9.4f, 28.7f),
                         .ViewerFrameReach = plain.SplitFar[plain.Count - 1]});
    f32 worstCarried = 0.0f;
    for (u32 k = 0; k < all.Count; ++k)
    {
        worstCarried =
            std::max(worstCarried, glm::compMax(glm::abs(TexelCoord(all, k, carried, 1024) -
                                                         TexelCoord(shifted, k, carried, 1024))));
    }
    CHECK(worstCarried < 1e-3f);
    CHECK(plain.ViewerFrameCount == 0);
}

TEST_CASE("ComputeCascades: the radius ladder holds a hovering need and grows at once")
{
    // One cascade, so the far plane alone sets the need. Find the far plane at which the step
    // first rises, then hover half a percent either side of it.
    const vec3 lightDir(0.3f, -1.0f, 0.2f);
    const CascadeSettings settings{.Count = 1, .Resolution = 1024};
    const vec3 eye(0.2f, 2.0f, 3.0f);
    const vec3 forward = glm::normalize(vec3(0.1f, -0.3f, -1.0f));
    const auto fit = [&](const f32 far, const CascadeData* previous)
    {
        return ComputeCascades(MakeCamera(eye, forward, 0.1f, far), lightDir, AABB::Empty(),
                               settings, {.Previous = previous});
    };

    const i32 lowStep = fit(10.0f, nullptr).RadiusStep[0];
    f32 below = 10.0f;
    f32 above = 20.0f;
    REQUIRE(fit(above, nullptr).RadiusStep[0] > lowStep);
    for (int i = 0; i < 40; ++i)
    {
        const f32 mid = 0.5f * (below + above);
        (fit(mid, nullptr).RadiusStep[0] > lowStep ? above : below) = mid;
    }

    CascadeData held = fit(above * 1.005f, nullptr);
    const i32 highStep = held.RadiusStep[0];
    REQUIRE(fit(above * 0.995f, nullptr).RadiusStep[0] < highStep);
    u32 changes = 0;
    f32 margin = std::numeric_limits<f32>::max();
    for (int i = 0; i < 12; ++i)
    {
        const f32 far = above * (i % 2 == 0 ? 0.995f : 1.005f);
        held = fit(far, &held);
        changes += held.RadiusStep[0] == highStep ? 0u : 1u;
        margin = std::min(margin, SliceMarginTexels(held, MakeCamera(eye, forward, 0.1f, far),
                                                    settings.Resolution));
    }
    CHECK(changes == 0);

    // A need past the held step, either way, takes its own step at once.
    const CascadeData grown = fit(above * 1.5f, &held);
    CHECK(grown.RadiusStep[0] == fit(above * 1.5f, nullptr).RadiusStep[0]);
    CHECK(grown.RadiusStep[0] > highStep);
    margin = std::min(margin, SliceMarginTexels(grown, MakeCamera(eye, forward, 0.1f, above * 1.5f),
                                                settings.Resolution));
    const CascadeData shrunk = fit(above * 0.5f, &held);
    CHECK(shrunk.RadiusStep[0] == fit(above * 0.5f, nullptr).RadiusStep[0]);
    CHECK(shrunk.RadiusStep[0] < highStep - 1);

    // The held radius never falls below the need: every slice still lands inside its tile.
    CHECK(margin >= 1.0f);
}

TEST_CASE("SceneFloatingOrigin: reads the scene's component, or a fixed origin without one")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    const Unique<Scene> scene = Scene::Create(types);

    const FloatingOrigin none = SceneFloatingOrigin(*scene);
    CHECK(none.Position == dvec3(0.0));
    CHECK(none.ViewerFrameReach == 0.0f);
    CHECK(CountFloatingOrigins(*scene) == 0);

    const Entity entity = scene->CreateEntity();
    scene->Add<FloatingOrigin>(entity) = {.Position = dvec3(1.0e5, -2.5, 3.0e4),
                                          .ViewerFrameReach = 12.0f};
    const FloatingOrigin read = SceneFloatingOrigin(*scene);
    CHECK(read.Position == dvec3(1.0e5, -2.5, 3.0e4));
    CHECK(read.ViewerFrameReach == 12.0f);
    CHECK(CountFloatingOrigins(*scene) == 1);
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

TEST_CASE(
    "ComputeCascades: a fitted cascade's XY extent tracks the visible slab, not the far plane")
{
    // A camera whose far plane (1000) sits far past the scene. The split range fits the
    // scene, and each cascade's XY extent is the camera frustum slice at the cascade's
    // *fitted* depth: the shadow sub-frustum's near/far corners are placed at the fitted
    // range first, and cascade slices interpolate between those — so the last cascade's far
    // edge sits at the fitted far, not the 1000-unit far plane. Placing the sub-frustum
    // against the whole camera range instead would blow the last cascade's extent up ~16x
    // and shrink the scene to a tiny corner of the atlas tile.
    const CameraView camera = MakeTestCamera(0.1f, 1000.0f);
    const vec3 lightDir(0.3f, -1.0f, 0.2f);
    const CascadeSettings settings{};

    const AABB sceneBounds{.Min = vec3(-15.0f, -2.0f, -15.0f), .Max = vec3(15.0f, 8.0f, 15.0f)};
    const CascadeData data = ComputeCascades(camera, lightDir, sceneBounds, settings);

    // Project a 15-unit horizontal world span (well within the scene) through the last
    // cascade and measure how much of the NDC tile it covers. A tightly fit cascade
    // (extent ~tens of units) gives a large NDC span; an extent inflated to the far-plane
    // slice (~1150 units wide) squishes the same span to a few percent of the tile.
    const u32 last = data.Count - 1;
    const vec4 a = data.ViewProj[last] * vec4(0.0f, 0.0f, 0.0f, 1.0f);
    const vec4 b = data.ViewProj[last] * vec4(15.0f, 0.0f, 0.0f, 1.0f);
    const f32 ndcSpan = glm::length(vec2(vec3(b) / b.w) - vec2(vec3(a) / a.w));
    CHECK(ndcSpan > 0.1f);
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
    // near plane (reverse-Z: ndc.z > 1).
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

TEST_CASE("ComputeCascades: MaxDistance caps the far split")
{
    const CameraView camera = MakeTestCamera(0.1f, 1000.0f);
    const vec3 lightDir(0.3f, -1.0f, 0.2f);

    const CascadeData uncapped =
        ComputeCascades(camera, lightDir, AABB::Empty(), CascadeSettings{});
    CHECK(uncapped.SplitFar[uncapped.Count - 1] == camera.GetFar());

    const CascadeData capped =
        ComputeCascades(camera, lightDir, AABB::Empty(), CascadeSettings{.MaxDistance = 100.0f});
    CHECK(capped.SplitFar[capped.Count - 1] == doctest::Approx(100.0f));
    for (u32 i = 1; i < capped.Count; ++i)
    {
        CHECK(capped.SplitFar[i] > capped.SplitFar[i - 1]);
    }

    // A cap the visible range already sits inside changes nothing.
    const CascadeData slack =
        ComputeCascades(camera, lightDir, AABB::Empty(), CascadeSettings{.MaxDistance = 5000.0f});
    CHECK(slack.SplitFar[slack.Count - 1] == camera.GetFar());

    // A degenerate cap at or below the near plane is ignored rather than asserting.
    const CascadeData tiny =
        ComputeCascades(camera, lightDir, AABB::Empty(), CascadeSettings{.MaxDistance = 0.05f});
    CHECK(tiny.SplitFar[tiny.Count - 1] == camera.GetFar());
}

TEST_CASE("ComputeCascades: MinDistance floors the shadow near for a tiny-near camera")
{
    // A reverse-Z far-field camera: a micron near keeps far-field depth precision while the
    // far plane sits astronomically far, and the receivers are a small bound the camera sits
    // inside (a cockpit). Without a shadow-near floor the near cascades pack into a sub-pixel
    // shell at the eye; MinDistance decouples the cascade fit from the render near so the
    // cascades stay well-formed and fit the receivers that matter.
    CameraView camera;
    camera.SetPerspective(glm::radians(60.0f), 16.0f / 9.0f, 1e-6f, 20000.0f);
    camera.SetView(vec3(0.0f, 0.0f, 0.0f), vec3(0.0f, 0.0f, -1.0f), vec3(0.0f, 1.0f, 0.0f));
    const vec3 lightDir(0.3f, -1.0f, 0.2f);
    const AABB cockpit{.Min = vec3(-3.0f, -3.0f, -3.0f), .Max = vec3(3.0f, 3.0f, 3.0f)};

    const CascadeData floored =
        ComputeCascades(camera, lightDir, cockpit, CascadeSettings{.MinDistance = 0.1f});

    // Every cascade is well-formed: finite matrices, a positive depth range and texel size —
    // none collapsed onto the eye.
    for (u32 k = 0; k < floored.Count; ++k)
    {
        CHECK(IsFinite(floored.ViewProj[k]));
        CHECK(floored.DepthRange[k] > 0.0f);
        CHECK(floored.TexelWorldSize[k] > 0.0f);
    }
    // The first cascade already covers past the floored near — no cascade is spent on the
    // sub-floor shell at the eye — and the splits stay strictly increasing.
    CHECK(floored.SplitFar[0] > 0.1f);
    for (u32 i = 1; i < floored.Count; ++i)
    {
        CHECK(floored.SplitFar[i] > floored.SplitFar[i - 1]);
    }

    // The floor raises the near split: with no floor the first cascade's far collapses toward
    // the micron near, so the near cascades sit in a shell no receiver occupies.
    const CascadeData unfloored = ComputeCascades(camera, lightDir, cockpit, CascadeSettings{});
    CHECK(floored.SplitFar[0] > unfloored.SplitFar[0]);
}

TEST_CASE("ComputeCascades: below the MinDistance floor the fit ignores the render near")
{
    // Once MinDistance floors the shadow near, the render near only names the camera's depth
    // precision, so any two nears under the floor must fit the same cascades. The eye sits off
    // the origin on purpose: a micron near's corners lie closer to the eye than one float ulp at
    // a cockpit's distance from the origin, so a fit that differences them against the eye
    // reads rounding as the frustum's shape.
    const vec3 eye(1.3f, 6.2f, -11.2f);
    const auto makeCamera = [&eye](const f32 near)
    {
        CameraView camera;
        camera.SetPerspective(glm::radians(60.0f), 16.0f / 9.0f, near, 20000.0f);
        camera.SetView(eye, eye + vec3(0.3f, -0.1f, -1.0f), vec3(0.0f, 1.0f, 0.0f));
        return camera;
    };
    const vec3 lightDir(0.96f, -0.16f, 0.24f);
    const AABB hull{.Min = vec3(-12.0f, -2.0f, -20.0f), .Max = vec3(12.0f, 8.0f, 4.0f)};
    const CascadeSettings settings{.MaxDistance = 3000.0f, .MinDistance = 0.1f};

    const CascadeData micron = ComputeCascades(makeCamera(1e-6f), lightDir, hull, settings);
    const CascadeData centimetre = ComputeCascades(makeCamera(1e-2f), lightDir, hull, settings);

    REQUIRE(micron.Count == centimetre.Count);
    f32 worstSplit = 0.0f;
    f32 worstMatrix = 0.0f;
    for (u32 k = 0; k < micron.Count; ++k)
    {
        worstSplit = std::max(worstSplit, std::abs(micron.SplitFar[k] - centimetre.SplitFar[k]) /
                                              centimetre.SplitFar[k]);
        for (int c = 0; c < 4; ++c)
        {
            for (int r = 0; r < 4; ++r)
            {
                const f32 a = micron.ViewProj[k][c][r];
                const f32 b = centimetre.ViewProj[k][c][r];
                worstMatrix = std::max(worstMatrix, std::abs(a - b) / std::max(1.0f, std::abs(b)));
            }
        }
    }
    CHECK(worstSplit < 1e-4f);
    CHECK(worstMatrix < 1e-3f);
}

TEST_CASE("ComputeCascades: without PancakeNear the cull matrix equals the render matrix")
{
    const CameraView camera = MakeTestCamera();
    const AABB tall{.Min = vec3(-20.0f, -1.0f, -20.0f), .Max = vec3(20.0f, 30.0f, 20.0f)};
    const CascadeData data =
        ComputeCascades(camera, vec3(0.3f, -1.0f, 0.2f), tall, CascadeSettings{});

    for (u32 k = 0; k < data.Count; ++k)
    {
        for (int c = 0; c < 4; ++c)
        {
            for (int r = 0; r < 4; ++r)
            {
                CHECK(data.ViewProj[k][c][r] == data.CullViewProj[k][c][r]);
            }
        }
    }
}

TEST_CASE("ComputeCascades: PancakeNear keeps the render near tight and the cull near extended")
{
    const CameraView camera = MakeTestCamera(0.1f, 100.0f);
    const vec3 lightDir(0.3f, -1.0f, 0.2f);

    // A bound tall along the light axis: a caster high above the frustum slice.
    const AABB tall{.Min = vec3(-20.0f, -1.0f, -20.0f), .Max = vec3(20.0f, 30.0f, 20.0f)};
    const CascadeData pancaked =
        ComputeCascades(camera, lightDir, tall, CascadeSettings{.PancakeNear = true});
    const CascadeData extended = ComputeCascades(camera, lightDir, tall, CascadeSettings{});

    // The high caster falls before the pancaked render near (reverse-Z: ndc.z > 1 — the
    // depth clamp flattens it at raster time) but inside the cull matrix's depth range.
    const vec3 highCaster(0.0f, 25.0f, 0.0f);
    const vec4 renderClip = pancaked.ViewProj[0] * vec4(highCaster, 1.0f);
    CHECK(vec3(renderClip).z / renderClip.w > 1.0f);
    const vec4 cullClip = pancaked.CullViewProj[0] * vec4(highCaster, 1.0f);
    const f32 cullZ = vec3(cullClip).z / cullClip.w;
    CHECK(cullZ >= 0.0f);
    CHECK(cullZ <= 1.0f);

    // The tight render range is what pancaking buys: a fraction of the extended one.
    CHECK(pancaked.DepthRange[0] < extended.DepthRange[0]);

    // XY fit is untouched: only the near plane differs between the two modes. The
    // NDC-x-per-world rate is the matrix's first row (glm [c][0] across columns).
    const auto xRow = [](const mat4& m) { return glm::length(vec3(m[0][0], m[1][0], m[2][0])); };
    CHECK(xRow(pancaked.ViewProj[0]) == doctest::Approx(xRow(extended.ViewProj[0])));
}

TEST_CASE("ComputeCascades: TexelWorldSize and DepthRange are positive and texel-consistent")
{
    const CameraView camera = MakeTestCamera(0.1f, 100.0f);
    const CascadeSettings settings{.Resolution = 1024, .PancakeNear = true};
    const CascadeData data =
        ComputeCascades(camera, vec3(0.3f, -1.0f, 0.2f), AABB::Empty(), settings);

    for (u32 k = 0; k < data.Count; ++k)
    {
        CHECK(data.TexelWorldSize[k] > 0.0f);
        CHECK(data.DepthRange[k] > 0.0f);
        // The box is exactly Resolution texels of the reported pitch, recovered from the
        // projection scale (orthoZO: proj[0][0] = 2/width, so width = 2/|X row|, row 0 = glm
        // [c][0]).
        const f32 width = 2.0f / glm::length(vec3(data.ViewProj[k][0][0], data.ViewProj[k][1][0],
                                                  data.ViewProj[k][2][0]));
        const f32 expectedWidth = data.TexelWorldSize[k] * static_cast<f32>(settings.Resolution);
        CHECK(width == doctest::Approx(expectedWidth).epsilon(0.001));
    }

    // Cascades cover progressively more world per texel.
    for (u32 k = 1; k < data.Count; ++k)
    {
        CHECK(data.TexelWorldSize[k] > data.TexelWorldSize[k - 1]);
    }
}

TEST_CASE("ComputeShadowAtlasGrid: tile layout scales with cascade count")
{
    // 1 -> 1x1, 2 -> 2x1, 3/4 -> 2x2; counts clamp to [1, MaxCascades].
    CHECK(ComputeShadowAtlasGrid(1, 1).Columns == 1);
    CHECK(ComputeShadowAtlasGrid(1, 1).Rows == 1);
    CHECK(ComputeShadowAtlasGrid(2, 1).Columns == 2);
    CHECK(ComputeShadowAtlasGrid(2, 1).Rows == 1);
    CHECK(ComputeShadowAtlasGrid(3, 1).Columns == 2);
    CHECK(ComputeShadowAtlasGrid(3, 1).Rows == 2);
    CHECK(ComputeShadowAtlasGrid(4, 1).Columns == 2);
    CHECK(ComputeShadowAtlasGrid(4, 1).Rows == 2);
    // Clamp: 0 -> 1, oversized -> MaxCascades's layout.
    CHECK(ComputeShadowAtlasGrid(0, 1).Columns == 1);
    CHECK(ComputeShadowAtlasGrid(99, 1).Columns == 2);
}

TEST_CASE("ComputeShadowAtlasGrid: cascade sets stack as row bands and clamp to MaxCascadeSets")
{
    // A set's own layout does not change with the set count — the sets stack, so only the
    // total row count grows. Columns stay put, which is what keeps a cascade's column index
    // independent of which set it belongs to.
    const ShadowAtlasGrid one = ComputeShadowAtlasGrid(4, 1);
    const ShadowAtlasGrid two = ComputeShadowAtlasGrid(4, 2);
    CHECK(one.TotalRows() == one.Rows);
    CHECK(two.Columns == one.Columns);
    CHECK(two.Rows == one.Rows);
    CHECK(two.TotalRows() == 2 * one.Rows);
    // Clamp: 0 -> 1 set, oversized -> MaxCascadeSets.
    CHECK(ComputeShadowAtlasGrid(4, 0).Sets == 1);
    CHECK(ComputeShadowAtlasGrid(4, 99).Sets == MaxCascadeSets);
}

TEST_CASE("ComposeTileRemap: a 1x1 grid leaves the matrix unchanged")
{
    // One cascade fills the whole atlas, so its remap is the identity.
    const mat4 vp = glm::perspective(glm::radians(60.0f), 1.5f, 0.1f, 50.0f);
    const mat4 remapped = ComposeTileRemap(vp, 0, 0, ComputeShadowAtlasGrid(1, 1));
    for (int c = 0; c < 4; ++c)
    {
        for (int r = 0; r < 4; ++r)
        {
            CHECK(remapped[c][r] == doctest::Approx(vp[c][r]));
        }
    }
}

TEST_CASE("ComposeTileRemap: each cascade's NDC center maps to its tile center in a 2x2 atlas")
{
    // With an identity view-proj, a clip point's NDC.xy passes straight through, so the
    // remap of the NDC center (0,0) is exactly the tile center. Cascade k -> tile
    // (k % 2, k / 2): centers at the four ±0.5 quadrant midpoints.
    const mat4 id(1.0f);
    const vec4 center(0.0f, 0.0f, 0.25f, 1.0f);

    struct Expect
    {
        u32 cascade;
        f32 x;
        f32 y;
    };
    for (const Expect& e :
         {Expect{.cascade = 0, .x = -0.5f, .y = -0.5f}, Expect{.cascade = 1, .x = 0.5f, .y = -0.5f},
          Expect{.cascade = 2, .x = -0.5f, .y = 0.5f}, Expect{.cascade = 3, .x = 0.5f, .y = 0.5f}})
    {
        const vec4 p = ComposeTileRemap(id, e.cascade, 0, ComputeShadowAtlasGrid(4, 1)) * center;
        CHECK(p.x == doctest::Approx(e.x));
        CHECK(p.y == doctest::Approx(e.y));
        // Z and W are untouched: the depth compare is tile-agnostic.
        CHECK(p.z == doctest::Approx(0.25f));
        CHECK(p.w == doctest::Approx(1.0f));
    }
}

TEST_CASE("ComposeTileRemap: the NDC extent maps fully inside the cascade's tile")
{
    // Cascade 0 of a 2x2 atlas owns the lower-left quadrant: NDC x in [-1,0]. The two
    // NDC.x corners (-1, +1) map to the tile's left and right edges (-1, 0).
    const mat4 id(1.0f);
    const ShadowAtlasGrid grid = ComputeShadowAtlasGrid(4, 1);
    const vec4 left = ComposeTileRemap(id, 0, 0, grid) * vec4(-1.0f, 0.0f, 0.0f, 1.0f);
    const vec4 right = ComposeTileRemap(id, 0, 0, grid) * vec4(1.0f, 0.0f, 0.0f, 1.0f);
    CHECK(left.x == doctest::Approx(-1.0f));
    CHECK(right.x == doctest::Approx(0.0f));
}

TEST_CASE("ComposeTileRemap: a second cascade set lands in its own atlas row band")
{
    // Two sets of four cascades tile a 2-column, 4-row atlas: set 0 owns rows 0-1 and set 1
    // rows 2-3. The same cascade index in the two sets therefore shares a column and differs
    // in row by exactly one set's height — the property that keeps the two sets from
    // overwriting each other's depth.
    const mat4 id(1.0f);
    const ShadowAtlasGrid grid = ComputeShadowAtlasGrid(4, 2);
    const vec4 center(0.0f, 0.0f, 0.25f, 1.0f);

    const vec4 first = ComposeTileRemap(id, 0, 0, grid) * center;
    const vec4 second = ComposeTileRemap(id, 0, 1, grid) * center;
    CHECK(first.x == doctest::Approx(second.x));
    // Four rows of NDC height 0.5 each: the band offset is two rows.
    CHECK(first.y == doctest::Approx(-0.75f));
    CHECK(second.y == doctest::Approx(0.25f));

    // Every tile of both sets stays inside the atlas, and no two share a centre.
    vector<vec2> centres;
    for (u32 set = 0; set < grid.Sets; ++set)
    {
        for (u32 k = 0; k < 4; ++k)
        {
            const vec4 p = ComposeTileRemap(id, k, set, grid) * center;
            CHECK(std::abs(p.x) <= 1.0f);
            CHECK(std::abs(p.y) <= 1.0f);
            centres.emplace_back(p.x, p.y);
        }
    }
    REQUIRE(centres.size() == 8);
    u32 duplicates = 0;
    for (usize a = 0; a < centres.size(); ++a)
    {
        for (usize b = a + 1; b < centres.size(); ++b)
        {
            duplicates += glm::length(centres[a] - centres[b]) < 1e-4f ? 1u : 0u;
        }
    }
    CHECK(duplicates == 0);
}
