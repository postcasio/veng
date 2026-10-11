// The lens-dirt mask's placement: its cover fit over the presented frame, and the bloom level the
// term reads. Both are device-free functions, pinned here rather than through a rendered frame.

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cmath>

#include "Renderer/BloomMips.h"
#include "Renderer/LensDirt.h"

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // The presented frames a mask must cover: 4:3 through 21:9, a portrait one, and the square.
    constexpr std::array<uvec2, 7> Frames{
        uvec2{1024, 768},  uvec2{1920, 1080}, uvec2{2560, 1600}, uvec2{3440, 1440},
        uvec2{2560, 1080}, uvec2{1080, 1920}, uvec2{1000, 1000},
    };

    // A square mask (the authored case), a wide one and a tall one.
    constexpr std::array<uvec2, 3> Masks{uvec2{2048, 2048}, uvec2{4096, 1024}, uvec2{512, 1536}};
}

TEST_CASE("The lens-dirt cover fit keeps the mask's aspect, centres it and covers the frame")
{
    f32 worstAspect = 0.0f;
    f32 worstCentre = 0.0f;
    f32 worstSpanShort = 0.0f;
    f32 worstOutside = 0.0f;
    for (const uvec2 frame : Frames)
    {
        for (const uvec2 mask : Masks)
        {
            const vec4 map = ComputeLensDirtCoverMap(frame, mask);
            // The mask UVs the frame's corners land on.
            const vec2 lo = vec2(map.z, map.w);
            const vec2 hi = vec2(map.x, map.y) + lo;
            const vec2 span = hi - lo;

            // Aspect preserved: the mask region shown has the frame's shape, in texels.
            const f32 shownAspect =
                (span.x * static_cast<f32>(mask.x)) / (span.y * static_cast<f32>(mask.y));
            const f32 frameAspect = static_cast<f32>(frame.x) / static_cast<f32>(frame.y);
            worstAspect = std::max(worstAspect, std::abs(shownAspect / frameAspect - 1.0f));

            // Centred: the shown region's middle is the mask's middle.
            worstCentre = std::max(worstCentre, glm::length((lo + hi) * 0.5f - vec2(0.5f)));

            // Covers: no frame pixel reads outside the mask, and one axis spans it exactly, so
            // the fit is the largest that covers rather than merely one that does.
            worstOutside = std::max({worstOutside, -lo.x, -lo.y, hi.x - 1.0f, hi.y - 1.0f});
            worstSpanShort = std::max(worstSpanShort, 1.0f - std::max(span.x, span.y));
        }
    }
    CHECK(worstAspect < 1e-5f);
    CHECK(worstCentre < 1e-6f);
    CHECK(worstOutside <= 1e-6f);
    CHECK(worstSpanShort < 1e-6f);
}

TEST_CASE("A frame of the mask's own shape shows the whole mask")
{
    const vec4 map = ComputeLensDirtCoverMap(uvec2{1920, 1080}, uvec2{3840, 2160});
    CHECK(map.x == doctest::Approx(1.0f));
    CHECK(map.y == doctest::Approx(1.0f));
    CHECK(map.z == doctest::Approx(0.0f));
    CHECK(map.w == doctest::Approx(0.0f));
}

TEST_CASE("The lens-dirt term reads a wide level a fixed distance from the coarsest")
{
    // The level spans the same share of the frame wherever the pyramid's coarsest does: three
    // levels above it, and level 0 on a pyramid too short to have one.
    for (const uvec2 frame : {uvec2{1920, 1080}, uvec2{3840, 2160}, uvec2{1280, 720}})
    {
        const u32 count = BloomMipCount(frame);
        REQUIRE(count > LensDirtLevelsAboveCoarsest);
        CHECK(LensDirtBloomLevel(count) == count - 1 - LensDirtLevelsAboveCoarsest);
    }
    CHECK(LensDirtBloomLevel(1) == 0);
    CHECK(LensDirtBloomLevel(LensDirtLevelsAboveCoarsest + 1) == 0);

    // At 1080p the level is a sixteenth of the frame across.
    const uvec2 base = BloomPyramidBase(uvec2{1920, 1080});
    const u32 level = LensDirtBloomLevel(BloomMipCount(uvec2{1920, 1080}));
    CHECK((base.x >> level) == 1920 / 16);
}
