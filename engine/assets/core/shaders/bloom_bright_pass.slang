// The bloom downsample's shared push block and its level-0 tap conditioning, included by both
// kernels (bloom_down.comp, bloom_down_kawase.comp): the two pipelines are built against one
// layout, and the bright pass has to treat a tap the same whichever kernel gathers it. Mirrors the
// CPU BloomDownPush (BloomPyramid.cpp) field for field.
#pragma once

struct PushConstants
{
    uint2  DestExtent;     // destination mip dimensions in texels (the valid sub-rect)
    float2 SourceScaleUV;  // source validExtent/allocExtent (dynamic-resolution sub-rect)
    float2 SourceMaxUV;    // source (validExtent-0.5)/allocExtent (bilinear-tap clamp)
    float  BrightPass;     // 1.0 = level-0 tap ceiling + bright-pass, 0.0 = plain downsample
    float  Threshold;      // bright-pass luminance knee, in source units (level 0 only)
    uint   MaskTexture;    // bindless slot of the bloom-mask target (level 0 only)
    uint   MaskSampler;    // bindless slot of the sampler the mask is read through
    uint   MaskEnabled;    // 1 = fold the mask in, 0 = the two slots above are unset
    float  FireflyCeiling; // the most luminance one tap carries, in source units (level 0 only)
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

// Rec. 709 relative luminance.
float Luma(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// True where every component is finite. Tested on the exponent bits rather than through
// isnan/isinf, which a fast-math shader compile is free to fold away.
bool IsFinite(float3 c)
{
    const uint3 exponent = asuint(c) & 0x7F800000u;
    return all(exponent != 0x7F800000u);
}

// One level-0 tap, conditioned before it is averaged: a non-finite tap contributes nothing, and a
// tap brighter than half the ceiling is rolled off toward the ceiling along a smooth shoulder,
// scaled as a whole so its hue is kept. Every tap at or under half the ceiling passes unchanged,
// so the average of ordinary content stays linear — a small source seeds the same light wherever
// it falls on the grid — while no single tap, however extreme, carries more than the ceiling.
float3 ConditionTap(float3 c)
{
    if (!IsFinite(c))
    {
        return float3(0.0, 0.0, 0.0);
    }
    const float knee = 0.5 * max(g_PC.FireflyCeiling, 1e-4);
    const float luma = Luma(c);
    if (luma <= knee)
    {
        return c;
    }
    // The shoulder has unit slope at the knee and approaches knee + knee, the ceiling.
    const float over = luma - knee;
    const float limited = knee + over / (1.0 + over / knee);
    return c * (limited / luma);
}

// One level-0 mask tap: a non-finite mask contributes nothing. The mask is an authored amplitude,
// not a radiance, so it takes no ceiling.
float ConditionMask(float m)
{
    return IsFinite(float3(m, m, m)) ? m : 0.0;
}
