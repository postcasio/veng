// The anamorphic streak's row filter, run on one row on the CPU exactly as its shader runs it. The
// claims: the filter keeps a source's light away from the row's ends, spreads it symmetrically and
// falling monotonically from the source, spreads it further for a longer length, and with a
// positive chromatic spread keeps red and blue nearer the source than green; and the streak buffer
// is a quarter of the frame's width and half its height.

#include <doctest/doctest.h>

#include <cmath>
#include <vector>

#include "Renderer/StreakKernel.h"

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr i32 Width = 1280;
    constexpr i32 Centre = Width / 2;

    // A unit impulse at the centre of a row, filtered at one channel's ratio.
    std::vector<f32> FilteredImpulse(const f32 ratio)
    {
        std::vector<f32> row(static_cast<usize>(Width), 0.0f);
        row[static_cast<usize>(Centre)] = 1.0f;
        vector<f32> scratch;
        StreakFilterRow(row, ratio, scratch);
        return row;
    }

    // The mean squared distance of a row's light from the centre.
    f32 SecondMoment(const std::vector<f32>& row)
    {
        f32 moment = 0.0f;
        f32 total = 0.0f;
        for (i32 x = 0; x < Width; x++)
        {
            const f32 d = static_cast<f32>(x - Centre);
            moment += row[static_cast<usize>(x)] * d * d;
            total += row[static_cast<usize>(x)];
        }
        return moment / total;
    }
}

TEST_CASE("The streak filter keeps a source's light and falls symmetrically from it")
{
    // A reach short enough against the row that the light lost past its ends is negligible.
    const vec3 ratio = StreakChannelRatio(0.02f, 0.0f, Width);
    const std::vector<f32> row = FilteredImpulse(ratio.y);

    f32 total = 0.0f;
    f32 worstAsymmetry = 0.0f;
    bool falls = true;
    for (i32 x = 0; x < Width; x++)
    {
        total += row[static_cast<usize>(x)];
        const i32 mirror = 2 * Centre - x;
        if (mirror >= 0 && mirror < Width)
        {
            worstAsymmetry = std::max(worstAsymmetry, std::abs(row[static_cast<usize>(x)] -
                                                               row[static_cast<usize>(mirror)]));
        }
        if (x > Centre)
        {
            falls = falls && row[static_cast<usize>(x)] <= row[static_cast<usize>(x - 1)];
        }
    }
    CHECK(total == doctest::Approx(1.0f).epsilon(1.0e-4));
    CHECK(worstAsymmetry < 1.0e-6f);
    CHECK(falls);
    // The light at one reach out is 1/e of the source's peak.
    const f32 reach = 0.02f * static_cast<f32>(Width);
    const i32 distance = static_cast<i32>(reach);
    CHECK(row[static_cast<usize>(Centre + distance)] / row[static_cast<usize>(Centre)] ==
          doctest::Approx(std::exp(-static_cast<f32>(distance) / reach)).epsilon(1.0e-3));
}

TEST_CASE("A longer streak spreads further, and a positive spread keeps red and blue nearer")
{
    const f32 shortSpread = SecondMoment(FilteredImpulse(StreakChannelRatio(0.01f, 0.0f, Width).y));
    const f32 longSpread = SecondMoment(FilteredImpulse(StreakChannelRatio(0.04f, 0.0f, Width).y));
    CHECK(longSpread > 4.0f * shortSpread);

    const vec3 ratio = StreakChannelRatio(0.04f, 0.4f, Width);
    CHECK(ratio.x == ratio.z);
    CHECK(SecondMoment(FilteredImpulse(ratio.x)) < SecondMoment(FilteredImpulse(ratio.y)));
    // A spread past the floor never takes a channel's reach to nothing.
    const f32 floored = StreakChannelRatio(0.04f, 5.0f, Width).x;
    CHECK(floored > 0.0f);
    CHECK(floored < 1.0f);
}

TEST_CASE("The streak buffer is a quarter of the frame's width and half its height")
{
    CHECK(StreakExtent(uvec2{1920, 1080}) == uvec2{480, 540});
    CHECK(StreakExtent(uvec2{3, 1}) == uvec2{1, 1});
    const vec4 full = StreakSampleMap(uvec2{1920, 1080}, uvec2{1920, 1080});
    CHECK(full.x == 1.0f);
    CHECK(full.y == 1.0f);
}
