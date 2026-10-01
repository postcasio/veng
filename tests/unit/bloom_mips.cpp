// Bloom pyramid arithmetic. The pyramid's level 0 is half the scene, its chain stops a fixed number
// of levels short of 1×1, and every dispatch maps its reads through a sub-rect map; all three are
// device-free functions, pinned here rather than through a rendered frame.

#include <doctest/doctest.h>

#include <algorithm>
#include <array>

#include "Renderer/BloomMips.h"

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // Odd, power-of-two-adjacent, degenerate and ordinary extents: the floor rule and the level
    // count are what an odd or one-texel axis would break.
    constexpr std::array<uvec2, 9> Extents{
        uvec2{1920, 1080}, uvec2{2560, 1664}, uvec2{2047, 2047}, uvec2{1023, 7}, uvec2{127, 127},
        uvec2{128, 128},   uvec2{16, 3},      uvec2{3, 5},       uvec2{1, 1},
    };
}

TEST_CASE("bloom mips: level 0 is the floor-half of the scene, and each level halves the last")
{
    u32 violations = 0;
    for (const uvec2 scene : Extents)
    {
        const uvec2 base = BloomPyramidBase(scene);
        if (base != uvec2{std::max(scene.x / 2, 1u), std::max(scene.y / 2, 1u)})
        {
            violations++;
        }
        const u32 count = BloomMipCount(scene);
        if (count < 1)
        {
            violations++;
        }
        for (u32 level = 1; level < count; level++)
        {
            const uvec2 above = ComputeMipSubRect(base, base, level - 1).ValidExtent;
            const uvec2 here = ComputeMipSubRect(base, base, level).ValidExtent;
            if (here.x < 1 || here.y < 1 || here != glm::max(above / 2u, uvec2(1)))
            {
                violations++;
            }
        }
    }
    CHECK(violations == 0);
}

TEST_CASE("bloom mips: the coarsest level holds an 8-15 texel edge whatever the extent")
{
    // The widest glow the pyramid reaches is set by its coarsest level, so that level's size is a
    // property of the chain, not of the extent it was built for. Below a 16-texel scene the chain is
    // the single half-extent level.
    for (const uvec2 scene : Extents)
    {
        CAPTURE(scene.x);
        CAPTURE(scene.y);
        const uvec2 base = BloomPyramidBase(scene);
        const u32 count = BloomMipCount(scene);
        const uvec2 coarsest = ComputeMipSubRect(base, base, count - 1).ValidExtent;
        const u32 edge = std::max(coarsest.x, coarsest.y);
        if (std::max(scene.x, scene.y) >= 16)
        {
            CHECK(edge >= 8);
            CHECK(edge < 16);
        }
        else
        {
            CHECK(count == 1);
            CHECK(coarsest == base);
        }
    }
}

TEST_CASE("bloom mips: a frame at its full allocation maps every read through the identity")
{
    // The bright pass reads the scene over the scene's allocation and every other dispatch reads the
    // pyramid over its own; at full resolution both maps cover the source exactly.
    for (const uvec2 scene : Extents)
    {
        const MipSubRect source = ComputeMipSubRect(scene, scene, 0);
        CHECK(source.ValidExtent == scene);
        CHECK(source.ScaleUV == vec2(1.0f));
        CHECK(source.MaxUV == (vec2(scene) - 0.5f) / vec2(scene));

        const uvec2 base = BloomPyramidBase(scene);
        const MipSubRect coarsest = ComputeMipSubRect(base, base, BloomMipCount(scene) - 1);
        CHECK(coarsest.ScaleUV == vec2(1.0f));
    }
}

TEST_CASE("bloom mips: a reduced valid extent maps inside the allocation, clamped half a texel in")
{
    const uvec2 alloc = BloomPyramidBase(uvec2{1920, 1080});
    const uvec2 valid = BloomPyramidBase(uvec2{960, 540});
    for (u32 level = 0; level < BloomMipCount(uvec2{1920, 1080}); level++)
    {
        CAPTURE(level);
        const MipSubRect map = ComputeMipSubRect(valid, alloc, level);
        CHECK(map.ScaleUV.x <= 1.0f);
        CHECK(map.ScaleUV.y <= 1.0f);
        CHECK(map.MaxUV.x < map.ScaleUV.x);
        CHECK(map.MaxUV.y < map.ScaleUV.y);
    }
}
