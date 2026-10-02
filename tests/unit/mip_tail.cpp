// The coarse-tail split of a mip chain: which suffix of levels one workgroup computes in a single
// dispatch. The choice is device-free arithmetic over the allocated extent, so its properties are
// pinned here — the tail fits the shared-memory capacity, never starts before the caller's floor,
// never binds more levels than the shaders declare, is never a lone level, and is as long as those
// bounds allow.

#include <doctest/doctest.h>

#include <array>

#include "Renderer/MipTail.h"

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    struct Chain
    {
        uvec2 Base;
        u32 MipCount;
    };

    // A bloom-sized chain, a full hi-Z chain at an odd extent, an elongated chain whose levels
    // collapse to one texel on one axis, a chain small enough to fit whole, and a one-level chain.
    constexpr std::array<Chain, 5> Chains{
        Chain{.Base = {1728, 1117}, .MipCount = 8}, Chain{.Base = {125, 93}, .MipCount = 7},
        Chain{.Base = {2048, 8}, .MipCount = 12},   Chain{.Base = {32, 32}, .MipCount = 6},
        Chain{.Base = {8, 8}, .MipCount = 1},
    };

    u64 TailTexels(const uvec2 base, const u32 first, const u32 mipCount)
    {
        u64 texels = 0;
        for (u32 level = first; level < mipCount; level++)
        {
            texels += MipTexelCount(base, level);
        }
        return texels;
    }
}

TEST_CASE("mip tail: the tail fits its capacity and its level bound, and is as long as they allow")
{
    constexpr std::array<u32, 3> Capacities{512, 3584, 7168};
    u32 violations = 0;
    u32 tails = 0;
    for (const Chain& chain : Chains)
    {
        for (const u32 capacity : Capacities)
        {
            for (const u32 minFirst : {0u, 1u})
            {
                const u32 first = MipTailFirstLevel(chain.Base, chain.MipCount, minFirst, capacity);
                if (first == chain.MipCount)
                {
                    // No tail: even the two coarsest levels past the floor do not fit.
                    const bool twoFit =
                        chain.MipCount >= minFirst + 2 &&
                        TailTexels(chain.Base, chain.MipCount - 2, chain.MipCount) <= capacity;
                    violations += twoFit ? 1 : 0;
                    continue;
                }
                tails++;
                const u32 length = chain.MipCount - first;
                const bool bounded = first >= minFirst && length >= 2 &&
                                     length <= MipTailMaxLevels &&
                                     TailTexels(chain.Base, first, chain.MipCount) <= capacity;
                // Maximal: one level earlier breaks a bound.
                const bool maximal = first == minFirst || length == MipTailMaxLevels ||
                                     TailTexels(chain.Base, first - 1, chain.MipCount) > capacity;
                violations += (bounded && maximal) ? 0 : 1;
            }
        }
    }
    CHECK(violations == 0u);
    CHECK(tails > 0u);
}

TEST_CASE("mip tail: a bloom-sized chain keeps its large levels and fuses its small ones")
{
    // 1728x1117 halves to 54x34, 27x17 and 13x8 at levels 5-7 (2,399 texels); level 4 (108x69)
    // would take it past 3,584.
    CHECK(MipTailFirstLevel({1728, 1117}, 8, 1, 3584) == 5u);
    // A chain that fits whole starts its tail at the floor.
    CHECK(MipTailFirstLevel({32, 32}, 6, 0, 7168) == 0u);
    CHECK(MipTailFirstLevel({32, 32}, 6, 1, 7168) == 1u);
    // A single level is never a tail.
    CHECK(MipTailFirstLevel({8, 8}, 1, 0, 7168) == 1u);
}
