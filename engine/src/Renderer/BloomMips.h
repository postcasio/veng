#pragma once

#include <Veng/Veng.h>

#include <algorithm>
#include <bit>

namespace Veng::Renderer
{
    /// @brief How many levels short of 1×1 the bloom pyramid stops, measured on the scene extent.
    ///
    /// The coarsest level then holds a ~8 px edge (2^3) rather than a degenerate 1×1 that
    /// contributes nothing.
    inline constexpr u32 BloomTileShift = 3;

    /// @brief The dynamic-resolution sub-rect mapping for one mip level of a chain allocated at
    /// its high-water mark.
    ///
    /// The valid extent at that level, and the (scale, clamp) UVs mapping a [0,1] valid UV into
    /// the level's valid region. At full resolution ScaleUV is 1 and MaxUV ~1.
    struct MipSubRect
    {
        /// @brief The valid extent at this level, floor-halved from the base and at least 1.
        uvec2 ValidExtent;
        /// @brief validExtent / allocExtent at this level.
        vec2 ScaleUV;
        /// @brief (validExtent - 0.5) / allocExtent at this level: the bilinear-tap clamp.
        vec2 MaxUV;
    };

    /// @brief The sub-rect map of one level of a chain whose level 0 is @p allocBase.
    /// @param validBase The valid extent at level 0.
    /// @param allocBase The allocated extent at level 0.
    /// @param level     The mip level the map is for.
    /// @return The level's valid extent and its (scale, clamp) UVs.
    [[nodiscard]] inline MipSubRect ComputeMipSubRect(const uvec2 validBase, const uvec2 allocBase,
                                                      const u32 level)
    {
        const uvec2 valid{std::max(validBase.x >> level, 1u), std::max(validBase.y >> level, 1u)};
        const uvec2 alloc{std::max(allocBase.x >> level, 1u), std::max(allocBase.y >> level, 1u)};
        return {
            .ValidExtent = valid,
            .ScaleUV = vec2(valid) / vec2(alloc),
            .MaxUV = (vec2(valid) - 0.5f) / vec2(alloc),
        };
    }

    /// @brief The extent of the bloom pyramid's level 0 for a scene extent: floor-halved, at least 1.
    ///
    /// The bright pass is the pyramid's first 2:1 downsample, so level 0 is the scene halved by
    /// the Vulkan mip rule. The same rule maps a frame's valid scene extent to the pyramid's valid
    /// base, so every pyramid-side sub-rect map is taken over this.
    /// @param sceneExtent The scene-colour extent the bright pass reads.
    /// @return The pyramid's level-0 extent.
    [[nodiscard]] inline uvec2 BloomPyramidBase(const uvec2 sceneExtent)
    {
        return {std::max(sceneExtent.x / 2, 1u), std::max(sceneExtent.y / 2, 1u)};
    }

    /// @brief The number of levels in the bloom pyramid for a scene extent.
    ///
    /// The chain is the full-extent chain stopping BloomTileShift levels short of 1×1, minus its
    /// finest level: with floor halving, level `j` of the half-base chain is level `j + 1` of the
    /// scene's, so the coarsest level is the same for every extent. At least one level.
    /// @param sceneExtent The scene-colour extent the bright pass reads.
    /// @return The pyramid's mip count.
    [[nodiscard]] inline u32 BloomMipCount(const uvec2 sceneExtent)
    {
        const i32 bits = static_cast<i32>(std::bit_width(std::max(sceneExtent.x, sceneExtent.y)));
        return static_cast<u32>(std::max(1, bits - static_cast<i32>(BloomTileShift) - 1));
    }
}
