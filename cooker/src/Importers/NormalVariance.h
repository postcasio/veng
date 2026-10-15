#pragma once

#include <Veng/Cook/Export.h>
#include <span>

#include <Veng/Veng.h>

namespace Veng::Cook
{
    /// @brief One level of a normal map's moments, the input the variance companion is built from.
    ///
    /// Per texel, RGBA f32 row-major: the mean of the unit normals the texel covers (xyz) and their
    /// spread about that mean (w), E|n − mean|². A source texel covers one normal, so its spread is
    /// zero; a mip texel's is what its level averaged away.
    struct NormalMoments
    {
        /// @brief Four floats per texel: mean x, y, z, then the spread.
        vector<f32> Texels;
        /// @brief Level width in texels.
        u32 Width = 0;
        /// @brief Level height in texels.
        u32 Height = 0;
    };

    /// @brief The unit normals of an eight-bit normal map, each with zero spread.
    ///
    /// X and Y read as UnpackNormal reads them (unsigned, times two minus one) and Z is
    /// reconstructed from them, so the normals measured are the ones a shader shades with.
    /// @param rgba    RGBA8 texels, row-major.
    /// @param width   Width in texels.
    /// @param height  Height in texels.
    VE_COOK_API NormalMoments NormalMomentsOfRgba8(std::span<const u8> rgba, u32 width, u32 height);

    /// @brief The unit normals of a float normal map, each with zero spread.
    ///
    /// The float counterpart of NormalMomentsOfRgba8, over channel values in the same unsigned
    /// encoding.
    /// @param rgba    RGBA f32 texels, row-major.
    /// @param width   Width in texels.
    /// @param height  Height in texels.
    NormalMoments NormalMomentsOfFloat(std::span<const f32> rgba, u32 width, u32 height);

    /// @brief Resamples a level to another extent, keeping the spread among the texels each covers.
    ///
    /// Filters the means and the second moments (spread plus squared mean length) linearly and
    /// takes the spread back out, so a downscaled base level carries the variation its downscale
    /// averaged away.
    /// @param source  The level to resample.
    /// @param width   Target width in texels.
    /// @param height  Target height in texels.
    VE_COOK_API NormalMoments ResampleNormalMoments(const NormalMoments& source, u32 width,
                                                    u32 height);

    /// @brief Halves a level with a 2x2 box filter, an odd edge clamping to its last column or row.
    ///
    /// A texel's spread is its four parents' mean spread plus the spread of their means about its
    /// own (the law of total variance), so it is exactly zero where the four agree.
    /// @param level      The level to halve.
    /// @param outWidth   The halved width, max(1, width / 2).
    /// @param outHeight  The halved height, max(1, height / 2).
    NormalMoments HalveNormalMoments(const NormalMoments& level, u32 outWidth, u32 outHeight);

    /// @brief The normal variance of every texel of a mip chain, levels largest-first.
    ///
    /// Level 0 is @p base itself and each further level halves the one before, matching the
    /// dimensions the texture cook gives its own mips (max(1, extent >> level)). Each value is
    /// NormalVarianceOfSpread of the texel's spread.
    /// @param base      The chain's level 0.
    /// @param mipCount  How many levels to produce.
    /// @return One row-major variance array per level.
    VE_COOK_API vector<vector<f32>> NormalVarianceChain(NormalMoments base, u32 mipCount);
}
