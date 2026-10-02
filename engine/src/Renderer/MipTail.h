#pragma once

#include <Veng/Veng.h>

#include <algorithm>

namespace Veng::Renderer
{
    /// @brief The most levels a fused mip-chain tail dispatch binds.
    ///
    /// The tail shaders declare this many storage-image bindings; a chain whose qualifying suffix is
    /// longer starts its tail later instead.
    inline constexpr u32 MipTailMaxLevels = 8;

    /// @brief The texel count of one level of a chain whose level 0 is @p base.
    /// @param base  The chain's level-0 extent.
    /// @param level The mip level.
    /// @return The level's texel count, each axis floor-halved and at least 1.
    [[nodiscard]] inline u32 MipTexelCount(const uvec2 base, const u32 level)
    {
        return std::max(base.x >> level, 1u) * std::max(base.y >> level, 1u);
    }

    /// @brief The first level of the coarse tail a single workgroup computes in one dispatch.
    ///
    /// A mip chain whose levels each depend on the whole of the level before pays one dispatch, and
    /// one barrier, per level — and its coarse levels are a handful of texels each, so those
    /// dispatches are nearly all overhead. The tail is the longest suffix of the chain whose levels
    /// together fit @p texelCapacity (the workgroup's shared memory) and number at most
    /// MipTailMaxLevels, starting no earlier than @p minFirst; one workgroup computes it level by
    /// level, synchronizing between levels instead of between dispatches. A suffix of fewer than two
    /// levels would replace no dispatch it does not also cost, so it is no tail.
    /// @param allocBase     The chain's allocated level-0 extent (the valid region never exceeds it,
    ///                      so a fit at the allocation fits every frame).
    /// @param mipCount      The chain's level count.
    /// @param minFirst      The earliest level the tail may start at (a level computed differently,
    ///                      such as a bright pass, stays out of it).
    /// @param texelCapacity The most texels the tail may hold in total.
    /// @return The tail's first level, or @p mipCount when the chain has no tail.
    [[nodiscard]] inline u32 MipTailFirstLevel(const uvec2 allocBase, const u32 mipCount,
                                               const u32 minFirst, const u32 texelCapacity)
    {
        u32 first = mipCount;
        u64 texels = 0;
        while (first > minFirst && mipCount - first < MipTailMaxLevels)
        {
            const u64 level = MipTexelCount(allocBase, first - 1);
            if (texels + level > texelCapacity)
            {
                break;
            }
            texels += level;
            first--;
        }
        return mipCount - first >= 2 ? first : mipCount;
    }
}
