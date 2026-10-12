// Screen-space reflections: the reflection denoiser's tunables and the helpers its three passes
// share. Ports ffx_denoiser_reflections_config.h and ffx_denoiser_reflections_common.h (its 32-bit
// path) from the FidelityFX SDK v1.1.4, with ffxRemapForWaveReduction from ffx_core.
//
// The reference's coordinate helpers flip y between UV and NDC (Direct3D's NDC is y-up); the
// engine's NDC is y-down like UV, so the flip is omitted, as in ssr_common.slang. ProjectPosition
// is the reflection chain's own (ssr_common.slang), which the denoiser's copy duplicates line for
// line.
//
// The reference tests a value for NaN and Inf with isnan/isinf; the engine's shader compile may
// fold those away under fast math, so DenoiserIsFinite tests the exponent bits instead, the same
// test written so it survives.
//
// This file is part of the FidelityFX SDK.
//
// Copyright (C) 2024 Advanced Micro Devices, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

#pragma once

#include "ssr_common.slang"

//=== ffx_denoiser_reflections_config.h ===

#define FFX_DNSR_REFLECTIONS_GAUSSIAN_K 3.0
#define FFX_DNSR_REFLECTIONS_RADIANCE_WEIGHT_BIAS 0.6
#define FFX_DNSR_REFLECTIONS_RADIANCE_WEIGHT_VARIANCE_K 0.1
#define FFX_DNSR_REFLECTIONS_AVG_RADIANCE_LUMINANCE_WEIGHT 0.3
#define FFX_DNSR_REFLECTIONS_PREFILTER_VARIANCE_WEIGHT 4.4
#define FFX_DNSR_REFLECTIONS_REPROJECT_SURFACE_DISCARD_VARIANCE_WEIGHT 1.5
#define FFX_DNSR_REFLECTIONS_PREFILTER_VARIANCE_BIAS 0.1
#define FFX_DNSR_REFLECTIONS_PREFILTER_NORMAL_SIGMA 512.0
#define FFX_DNSR_REFLECTIONS_PREFILTER_DEPTH_SIGMA 4.0
#define FFX_DNSR_REFLECTIONS_DISOCCLUSION_NORMAL_WEIGHT 1.4
#define FFX_DNSR_REFLECTIONS_DISOCCLUSION_DEPTH_WEIGHT 1.0
#define FFX_DNSR_REFLECTIONS_DISOCCLUSION_THRESHOLD 0.9
#define FFX_DNSR_REFLECTIONS_REPROJECTION_NORMAL_SIMILARITY_THRESHOLD 0.9999
#define FFX_DNSR_REFLECTIONS_SAMPLES_FOR_ROUGHNESS(r) (1.0 - exp(-r * 100.0))

//=== The constants of cbDenoiserReflections the chain's block does not already carry ===

// The reference's host sets the denoiser's render size to its context's window size: the extent
// the chain's resources are allocated at.
uint2 DenoiserRenderSize() { return g_Ssr.DenoiserRenderSize; }
float2 DenoiserInverseRenderSize() { return g_Ssr.DenoiserInverseRenderSize; }

//=== ffx_denoiser_reflections_common.h ===

bool FFX_DNSR_Reflections_IsGlossyReflection(float roughness) {
    return roughness < RoughnessThreshold();
}

bool FFX_DNSR_Reflections_IsMirrorReflection(float roughness) {
    return roughness < 0.0001;
}

// Mat must be able to transform origin from texture space to a linear space.
float3 FFX_DNSR_InvProjectPosition(float3 coord, float4x4 mat) {
    coord.xy = 2 * coord.xy - 1;
    float4 projected = mul(mat, float4(coord, 1));
    projected.xyz /= projected.w;
    return projected.xyz;
}

float FFX_DNSR_Reflections_GetLinearDepth(float2 uv, float depth) {
    const float3 view_space_pos = FFX_DNSR_InvProjectPosition(float3(uv, depth), InvProjection());
    return abs(view_space_pos.z);
}

float3 FFX_DNSR_Reflections_ScreenSpaceToViewSpace(float3 screen_uv_coord) {
    return FFX_DNSR_InvProjectPosition(screen_uv_coord, InvProjection());
}

float3 FFX_DNSR_Reflections_WorldSpaceToScreenSpacePrevious(float3 world_space_pos) {
    return ProjectPosition(world_space_pos, PrevViewProjection());
}

float3 FFX_DNSR_Reflections_ViewSpaceToWorldSpace(float4 view_space_coord) {
    return mul(InvView(), view_space_coord).xyz;
}

// Rounds value to the nearest multiple of 8
uint2 FFX_DNSR_Reflections_RoundUp8(uint2 value) {
    uint2 round_down = value & ~7u;    // 0b111;
    return select((round_down == value), value, value + 8);
}

float FFX_DNSR_Reflections_Luminance(float3 color)
{
    return max(float(dot(color, float3(0.299f, 0.587f, 0.114f))), float(0.001));
}

float FFX_DNSR_Reflections_ComputeTemporalVariance(float3 history_radiance, float3 radiance) {
    float history_luminance = FFX_DNSR_Reflections_Luminance(history_radiance);
    float luminance         = FFX_DNSR_Reflections_Luminance(radiance);
    float diff              = abs(history_luminance - luminance) / max(max(history_luminance, luminance), float(0.5f));
    return diff * diff;
}

uint FFX_DNSR_Reflections_PackFloat16(float2 v)
{
    uint2 p = uint2(f32tof16(v.x), f32tof16(v.y));
    return p.x | (p.y << 16);
}

float2 FFX_DNSR_Reflections_UnpackFloat16(uint a)
{
    float2 tmp = f16tof32(uint2(a & 0xFFFF, a >> 16));
    return tmp;
}

uint2 FFX_DNSR_Reflections_PackFloat16_4(float4 v) { return uint2(FFX_DNSR_Reflections_PackFloat16(v.xy), FFX_DNSR_Reflections_PackFloat16(v.zw)); }

float4 FFX_DNSR_Reflections_UnpackFloat16_4(uint2 a) { return float4(FFX_DNSR_Reflections_UnpackFloat16(a.x), FFX_DNSR_Reflections_UnpackFloat16(a.y)); }

// From "Temporal Reprojection Anti-Aliasing"
// https://github.com/playdeadgames/temporal
/**********************************************************************
Copyright (c) [2015] [Playdead]

Permission is hereby granted, free of charge, to any person obtaining a copy
of this software and associated documentation files (the "Software"), to deal
in the Software without restriction, including without limitation the rights
to use, copy, modify, merge, publish, distribute, sublicense, and/or sell
copies of the Software, and to permit persons to whom the Software is
furnished to do so, subject to the following conditions:

The above copyright notice and this permission notice shall be included in all
copies or substantial portions of the Software.

THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE
SOFTWARE.
********************************************************************/
float3 FFX_DNSR_Reflections_ClipAABB(float3 aabb_min, float3 aabb_max, float3 prev_sample) {
    // Main idea behind clipping - it prevents clustering when neighbor color space
    // is distant from history sample

    // Here we find intersection between color vector and aabb color box

    // Note: only clips towards aabb center
    float3 aabb_center = 0.5 * (aabb_max + aabb_min);
    float3 extent_clip = 0.5 * (aabb_max - aabb_min) + 0.001;

    // Find color vector
    float3 color_vector = prev_sample - aabb_center;
    // Transform into clip space
    float3 color_vector_clip = color_vector / extent_clip;
    // Find max absolute component
    color_vector_clip       = abs(color_vector_clip);
    float max_abs_unit = float(max(max(color_vector_clip.x, color_vector_clip.y), color_vector_clip.z));

    if (max_abs_unit > 1.0) {
        return float3(aabb_center + color_vector / max_abs_unit); // clip towards color vector
    } else {
        return float3(prev_sample); // point is inside aabb
    }
}

#ifdef FFX_DNSR_REFLECTIONS_ESTIMATES_LOCAL_NEIGHBORHOOD

#    ifndef FFX_DNSR_REFLECTIONS_LOCAL_NEIGHBORHOOD_RADIUS
#        define FFX_DNSR_REFLECTIONS_LOCAL_NEIGHBORHOOD_RADIUS 4
#    endif

float FFX_DNSR_Reflections_LocalNeighborhoodKernelWeight(float i) {
    const float radius = FFX_DNSR_REFLECTIONS_LOCAL_NEIGHBORHOOD_RADIUS + 1.0;
    return exp(-FFX_DNSR_REFLECTIONS_GAUSSIAN_K * (i * i) / (radius * radius));
}

#endif // FFX_DNSR_REFLECTIONS_ESTIMATES_LOCAL_NEIGHBORHOOD

//=== From ffx_core ===

uint DenoiserBitfieldExtract(uint src, uint off, uint bits)
{
    uint mask = (1u << bits) - 1;
    return (src >> off) & mask;
}

uint DenoiserBitfieldInsertMask(uint src, uint ins, uint bits)
{
    uint mask = (1u << bits) - 1;
    return (ins & mask) | (src & (~mask));
}

// ffxRemapForWaveReduction: local index -> 8×8 position, each 2×2 quad on four consecutive indices.
uint2 DenoiserRemapForWaveReduction(uint a)
{
    return uint2(DenoiserBitfieldInsertMask(DenoiserBitfieldExtract(a, 2u, 3u), a, 1u), DenoiserBitfieldInsertMask(DenoiserBitfieldExtract(a, 3u, 3u), DenoiserBitfieldExtract(a, 1u, 2u), 2u));
}

// any(isinf(v)) || any(isnan(v)) is !DenoiserIsFinite(v): an exponent of all ones is either.
bool DenoiserIsFinite(float3 v)
{
    const uint3 exponent = asuint(v) & 0x7F800000u;
    return all(exponent != 0x7F800000u);
}
