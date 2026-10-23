// The bloom downsample's shared push block and its level-0 tap conditioning, included by both
// kernels (bloom_down.comp, bloom_down_kawase.comp): the two pipelines are built against one
// layout, and the bright pass has to treat a tap the same whichever kernel gathers it. Mirrors the
// CPU BloomDownPush (BloomPyramid.cpp) field for field.
#pragma once

#include "bright_tap.slang"

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

// One level-0 tap, conditioned before it is averaged (ConditionBrightTap) under this pass's ceiling.
float3 ConditionTap(float3 c)
{
    return ConditionBrightTap(c, g_PC.FireflyCeiling);
}

// One level-0 mask tap: a non-finite mask contributes nothing. The mask is an authored amplitude,
// not a radiance, so it takes no ceiling.
float ConditionMask(float m)
{
    return IsFinite(float3(m, m, m)) ? m : 0.0;
}
