#pragma once

#include <algorithm>
#include <cmath>

#include <Veng/Asset/Types.h>

// The normal-variance companion's value: how far a normal map's normals spread across the texels a
// mip level's texel averages, expressed as the GGX width it adds. A filtered normal map averages its
// normals per mip but nothing widens the specular lobe to match, so a glossy, finely detailed
// surface glints under minification. The cook measures the spread each level averages away and
// stores it beside the map (CookedTextureHeader::VarianceFormat); a shader samples it with the
// normal map's handle and coordinates and widens the roughness by it (Toksvig's correction, in the
// additive alpha² form the geometric widening also takes).
//
// These are the C++ forms of the encode the cook and the decode and widening the shader header
// Veng/normal_variance.slang carry; the two are kept in step by hand.

namespace Veng
{
    /// @brief The GGX alpha² a footprint of unit normals adds, from the footprint's spread.
    ///
    /// @p spread is the footprint's total normal variance, E|n − mean|² over its unit normals,
    /// which is 1 − r² for the length r of the mean normal. Fitting a von Mises-Fisher lobe to the
    /// footprint (Banerjee et al.'s κ = (3r − r³) / (1 − r²)) and taking 2/κ gives the width the
    /// lobe must gain to cover it, as Han et al. 2007 and The Order: 1886 apply it; near zero it is
    /// the spread itself. The result saturates at 1, a fully rough lobe.
    /// @param spread  The footprint's total normal variance, in [0, 1].
    /// @return The alpha² to add, in [0, 1]; exactly zero for a zero spread.
    inline f32 NormalVarianceOfSpread(f32 spread)
    {
        const f32 s = std::clamp(spread, 0.0f, 1.0f);
        if (s <= 0.0f)
        {
            return 0.0f;
        }
        // 2/κ = 2s / (r (3 − r²)) with r² = 1 − s.
        const f32 denominator = std::sqrt(1.0f - s) * (2.0f + s);
        if (denominator <= 2.0f * s)
        {
            return 1.0f;
        }
        return 2.0f * s / denominator;
    }

    /// @brief The stored value of a normal variance: its fourth root, the roughness it alone is.
    ///
    /// The widening adds in alpha², which is roughness to the fourth, so storing the fourth root
    /// spends an eight-bit channel's precision where a glossy surface needs it, exactly as a
    /// roughness map does.
    /// @param variance  The alpha² to add (NormalVarianceOfSpread).
    /// @return The stored value, in [0, 1].
    inline f32 EncodeNormalVariance(f32 variance)
    {
        return std::sqrt(std::sqrt(std::clamp(variance, 0.0f, 1.0f)));
    }

    /// @brief The normal variance a stored value represents (the inverse of EncodeNormalVariance).
    /// @param stored  The sampled companion value, in [0, 1].
    /// @return The alpha² to add.
    inline f32 DecodeNormalVariance(f32 stored)
    {
        const f32 squared = stored * stored;
        return squared * squared;
    }

    /// @brief Widens a perceptual roughness by a normal variance: alpha'² = alpha² + scale · variance.
    ///
    /// The shader's WidenRoughnessByNormalVariance. A zero widening returns @p roughness untouched,
    /// so a surface whose normals do not spread shades bit for bit as without it.
    /// @param roughness  The authored perceptual roughness.
    /// @param variance   The alpha² the normals' spread adds (DecodeNormalVariance).
    /// @param scale      How much of it to apply; zero turns the widening off.
    /// @return The widened perceptual roughness, at least @p roughness (clamped to [0, 1]) and at
    ///         most 1.
    inline f32 WidenRoughnessByNormalVariance(f32 roughness, f32 variance, f32 scale)
    {
        const f32 added = scale * variance;
        if (!(added > 0.0f))
        {
            return roughness;
        }
        const f32 clamped = std::clamp(roughness, 0.0f, 1.0f);
        const f32 alpha = clamped * clamped;
        return std::sqrt(std::sqrt(std::min((alpha * alpha) + added, 1.0f)));
    }
}
