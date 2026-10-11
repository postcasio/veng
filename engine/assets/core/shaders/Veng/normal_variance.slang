// The normal-variance companion's encoding and the roughness widening it drives, binding-free so a
// compute kernel producing a companion and a material sampling one share it. Veng/surface.slang
// declares the companion array and the helpers that sample it.
//
// A filtered normal map averages its normals per mip, but nothing widens the specular lobe to match,
// so a glossy, finely detailed surface glints and sparkles under minification. A texture's companion
// stores, per texel of each level, the spread of the normals that level averaged away, as the GGX
// alpha² a lobe must gain to cover it (Toksvig's correction, in the von Mises-Fisher form). A
// material widens its roughness by it once, alpha'² = alpha² + scale · variance, before writing or
// shading — the same additive alpha² form the lighting core's geometric widening takes, which then
// adds the variation between pixels on top: the companion holds the spread inside a texel, the
// screen-space term the spread across one, so the two measure different scales and sum.
//
// The C++ forms of these functions are Veng/Asset/NormalVariance.h; the two are kept in step.

#ifndef VENG_NORMAL_VARIANCE_SLANG
#define VENG_NORMAL_VARIANCE_SLANG

// The alpha² a footprint of unit normals adds, from its spread E|n − mean|² (1 − r² for a mean
// normal of length r): 2/κ for the von Mises-Fisher fit κ = (3r − r³) / (1 − r²), saturating at a
// fully rough lobe. Zero for a zero spread.
float NormalVarianceOfSpread(float spread)
{
    const float s = saturate(spread);
    if (s <= 0.0)
    {
        return 0.0;
    }
    const float denominator = sqrt(1.0 - s) * (2.0 + s);
    if (denominator <= 2.0 * s)
    {
        return 1.0;
    }
    return 2.0 * s / denominator;
}

// The value a companion stores for a variance: its fourth root, the roughness the spread alone is,
// so an eight-bit channel spends its precision where a glossy surface needs it.
float EncodeNormalVariance(float variance)
{
    return sqrt(sqrt(saturate(variance)));
}

// The variance a stored companion value represents.
float DecodeNormalVariance(float stored)
{
    const float squared = stored * stored;
    return squared * squared;
}

// Widens a perceptual roughness by a normal variance: alpha'² = alpha² + scale · variance, bounded by
// a fully rough lobe. A zero widening returns the roughness untouched, so a surface whose normals do
// not spread shades exactly as without it.
float WidenRoughnessByNormalVariance(float roughness, float variance, float scale)
{
    const float added = scale * variance;
    if (!(added > 0.0))
    {
        return roughness;
    }
    const float clamped = saturate(roughness);
    const float alpha = clamped * clamped;
    return sqrt(sqrt(min(alpha * alpha + added, 1.0)));
}

#endif
