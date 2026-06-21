// TAA jitter sequence: pure CPU, no Context, no Vulkan. The Halton (2, 3) offsets
// must stay centered in the pixel ([-0.5, 0.5)), cover both axes, repeat on the
// declared period, and never produce the degenerate zero sample — the properties
// that make the jitter integrate to an unbiased supersample.

#include <doctest/doctest.h>

#include <Veng/Renderer/TaaJitter.h>

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
        CHECK(offset.x >= -0.5f);
        CHECK(offset.x < 0.5f);
        CHECK(offset.y >= -0.5f);
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
