// TAA jitter sequence: pure CPU, no Context, no Vulkan. The Halton (2, 3) offsets
// must stay within half a pixel of the centre, average to zero over a period, cover
// both axes, repeat on the declared period, and never produce the degenerate zero
// sample — the properties that make the jitter integrate to an unbiased supersample.
// Applied to a projection, the jitter moves every projected point by the same NDC
// offset at every depth, under a perspective and an orthographic camera alike.

#include <array>
#include <cmath>

#include <doctest/doctest.h>

#include <Veng/Renderer/TaaJitter.h>
#include <Veng/Scene/Camera.h>

using namespace Veng;
using namespace Veng::Renderer;

TEST_CASE("Halton values match the known sequence")
{
    // The canonical first terms of the radical-inverse sequence.
    CHECK(HaltonValue(1, 2) == doctest::Approx(0.5));
    CHECK(HaltonValue(2, 2) == doctest::Approx(0.25));
    CHECK(HaltonValue(3, 2) == doctest::Approx(0.75));
    CHECK(HaltonValue(1, 3) == doctest::Approx(1.0 / 3.0));
    CHECK(HaltonValue(2, 3) == doctest::Approx(2.0 / 3.0));
}

TEST_CASE("Jitter offsets stay centered in the pixel")
{
    for (u64 frame = 0; frame < 64; ++frame)
    {
        const vec2 offset = TaaJitterOffset(frame);
        CHECK(offset.x > -0.5f);
        CHECK(offset.x < 0.5f);
        CHECK(offset.y > -0.5f);
        CHECK(offset.y < 0.5f);
    }
}

TEST_CASE("Jitter sequence repeats on its period")
{
    for (u64 frame = 0; frame < TaaJitterSampleCount; ++frame)
    {
        const vec2 a = TaaJitterOffset(frame);
        const vec2 b = TaaJitterOffset(frame + TaaJitterSampleCount);
        CHECK(a.x == doctest::Approx(b.x));
        CHECK(a.y == doctest::Approx(b.y));
    }
}

TEST_CASE("Jitter covers distinct sub-pixel positions on both axes")
{
    // No phase is the degenerate (0, 0), and the sequence is not collapsed onto a
    // single axis — both components vary across the period.
    bool xVaries = false;
    bool yVaries = false;
    const vec2 first = TaaJitterOffset(0);
    for (u64 frame = 0; frame < TaaJitterSampleCount; ++frame)
    {
        const vec2 offset = TaaJitterOffset(frame);
        CHECK((offset.x != 0.0f || offset.y != 0.0f));
        if (offset.x != first.x)
        {
            xVaries = true;
        }
        if (offset.y != first.y)
        {
            yVaries = true;
        }
    }
    CHECK(xVaries);
    CHECK(yVaries);
}

TEST_CASE("A jitter period averages to the pixel centre")
{
    // A period whose offsets do not sum to zero displaces the integrated image by their mean.
    vec2 sum(0.0f);
    for (u64 frame = 0; frame < TaaJitterSampleCount; ++frame)
    {
        sum += TaaJitterOffset(frame);
    }
    const vec2 mean = sum / static_cast<f32>(TaaJitterSampleCount);
    CHECK(std::abs(mean.x) < 1e-3f);
    CHECK(std::abs(mean.y) < 1e-3f);
}

TEST_CASE("A jittered projection moves every depth by the same NDC offset")
{
    constexpr uvec2 Extent{1280, 720};

    CameraView perspective;
    perspective.SetPerspective(glm::radians(60.0f), 16.0f / 9.0f, 0.1f, 100.0f);
    perspective.SetView(vec3(1.0f, 2.0f, 3.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));
    CameraView orthographic;
    orthographic.SetOrthographic(8.0f, 4.5f, 0.1f, 100.0f);
    orthographic.SetView(vec3(1.0f, 2.0f, 3.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));

    // Three points on one view ray's neighbourhood at near, middle and far depths.
    const vec3 eye(1.0f, 2.0f, 3.0f);
    const vec3 forward = glm::normalize(-eye);
    const vec3 side = glm::normalize(glm::cross(forward, vec3(0.0f, 1.0f, 0.0f)));
    const std::array<vec3, 3> points{eye + forward * 0.5f + side * 0.1f,
                                     eye + forward * 10.0f + side * 2.0f,
                                     eye + forward * 90.0f - side * 20.0f};

    f32 worstError = 0.0f;
    f32 worstPixels = 0.0f;
    for (const CameraView* camera : {&perspective, &orthographic})
    {
        for (u64 frame = 0; frame < TaaJitterSampleCount; ++frame)
        {
            const vec2 jitterNdc = TaaJitterNdc(TaaJitterOffset(frame), Extent);
            const mat4 jittered =
                JitterProjection(camera->Projection(), jitterNdc) * camera->View();
            const mat4 plain = camera->ViewProjection();
            for (const vec3& point : points)
            {
                const vec4 a = jittered * vec4(point, 1.0f);
                const vec4 b = plain * vec4(point, 1.0f);
                const vec2 shift = vec2(a) / a.w - vec2(b) / b.w;
                worstError = std::max(worstError, glm::length(shift - jitterNdc));
                // Depth is untouched.
                worstError = std::max(worstError, std::abs(a.z / a.w - b.z / b.w));
            }
            const vec2 pixels = jitterNdc * vec2(Extent) * 0.5f;
            worstPixels = std::max({worstPixels, std::abs(pixels.x), std::abs(pixels.y)});
        }
    }
    CHECK(worstError < 1e-6f);
    CHECK(worstPixels <= 0.5f);
}
