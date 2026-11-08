// Screen-space reflections: the resources the reflection denoiser's passes bind and the loads and
// stores they reach them through. Ports ffx_denoiser_reflections_callbacks_hlsl.h (its 32-bit path)
// from the FidelityFX SDK v1.1.4.
//
// As in the reference, a pass defines the DENOISER_BIND_* names of the resources it binds before
// including this file, each to its binding in the pass's set 3; binding 0 is the linear sampler, and
// the chain's constants are a dynamic buffer bound after the block. The g-buffer's world normal and motion vectors are read through
// the bindless array by the indices in the push block, so their DENOISER_BIND_* names only say the
// pass reads them.
//
// What the engine's g-buffer settles at the load, where the reference takes host parameters:
// - the normal is a signed world normal, so the reference's unpack is the identity (mul 1, add 0);
// - roughness is already alpha (D3), so the perceptual square is never taken;
// - the motion vector is the engine's curUV - prevUV, so its scale is -1 on both axes (D4), giving
//   the reference's uv + motion = the previous frame's uv.
//
// A load past a texture's extent returns 0 and a store past it is dropped, the semantics the
// reference's loads and stores have on the threads of a tile's 16x16 apron and of the last row and
// column of tiles.
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

#include "ssr_denoiser_common.slang"

#if defined(DENOISER_BIND_SRV_INPUT_NORMAL) || defined(DENOISER_BIND_SRV_INPUT_MOTION_VECTORS)
#include "../Veng/bindless.slang"

struct DenoiserPushConstants
{
    uint NormalTexture;       // the g-buffer world normal's bindless index (G1)
    uint MotionVectorTexture; // the g-buffer motion vectors' bindless index (G3)
};

VE_PUSH_CONSTANTS(DenoiserPushConstants, g_PC);
#endif

bool DenoiserIsInside(int2 coordinate, uint2 extent)
{
    return all(uint2(coordinate) < extent);
}

uint2 DenoiserExtent(Texture2D<float> texture)
{
    uint width;
    uint height;
    texture.GetDimensions(width, height);
    return uint2(width, height);
}

uint2 DenoiserExtent(Texture2D<float3> texture)
{
    uint width;
    uint height;
    texture.GetDimensions(width, height);
    return uint2(width, height);
}

uint2 DenoiserExtent(Texture2D<float4> texture)
{
    uint width;
    uint height;
    texture.GetDimensions(width, height);
    return uint2(width, height);
}

// The pass's set-3 block, in binding order: the sampler, then each resource the pass defines a
// DENOISER_BIND_* name for. Every pass's bindings follow this declaration order.
struct DenoiserSet
{
    SamplerState s_LinearSampler;
    // SRVs
#if defined DENOISER_BIND_SRV_INPUT_DEPTH_HIERARCHY
    Texture2D<float> r_input_depth_hierarchy;
#endif
#if defined DENOISER_BIND_SRV_RADIANCE
    Texture2D<float4> r_radiance;
#endif
#if defined DENOISER_BIND_SRV_RADIANCE_HISTORY
    Texture2D<float4> r_radiance_history;
#endif
#if defined DENOISER_BIND_SRV_VARIANCE
    Texture2D<float> r_variance;
#endif
#if defined DENOISER_BIND_SRV_SAMPLE_COUNT
    Texture2D<float> r_sample_count;
#endif
#if defined DENOISER_BIND_SRV_AVERAGE_RADIANCE
    Texture2D<float3> r_average_radiance;
#endif
#if defined DENOISER_BIND_SRV_EXTRACTED_ROUGHNESS
    Texture2D<float> r_extracted_roughness;
#endif
#if defined DENOISER_BIND_SRV_DEPTH_HISTORY
    Texture2D<float> r_depth_history;
#endif
#if defined DENOISER_BIND_SRV_NORMAL_HISTORY
    Texture2D<float4> r_normal_history;
#endif
#if defined DENOISER_BIND_SRV_ROUGHNESS_HISTORY
    Texture2D<float> r_roughness_history;
#endif
#if defined DENOISER_BIND_SRV_REPROJECTED_RADIANCE
    Texture2D<float4> r_reprojected_radiance;
#endif

    // UAVs
#if defined DENOISER_BIND_UAV_RADIANCE
    [format("rgba16f")] RWTexture2D<float4> rw_radiance;
#endif
#if defined DENOISER_BIND_UAV_VARIANCE
    [format("r16f")] RWTexture2D<float> rw_variance;
#endif
#if defined DENOISER_BIND_UAV_SAMPLE_COUNT
    [format("r16f")] RWTexture2D<float> rw_sample_count;
#endif
#if defined DENOISER_BIND_UAV_AVERAGE_RADIANCE
    [format("r11f_g11f_b10f")] RWTexture2D<float3> rw_average_radiance;
#endif
#if defined DENOISER_BIND_UAV_DENOISER_TILE_LIST
    RWStructuredBuffer<uint> rw_denoiser_tile_list;
#endif
#if defined DENOISER_BIND_UAV_REPROJECTED_RADIANCE
    // The reference's float3 view of an RGBA16F target; nothing reads the fourth channel.
    [format("rgba16f")] RWTexture2D<float4> rw_reprojected_radiance;
#endif
};
VE_DESCRIPTOR_SET(3, DenoiserSet, g_Denoiser);

float3 LoadRadiance(int3 coordinate)
{
#if defined (DENOISER_BIND_SRV_RADIANCE)
    if (!DenoiserIsInside(coordinate.xy, DenoiserExtent(g_Denoiser.r_radiance)))
    {
        return float3(0.0);
    }
    return g_Denoiser.r_radiance.Load(coordinate).xyz;
#else
    return float3(0.0f);
#endif
}

float LoadVariance(int3 coordinate)
{
#if defined (DENOISER_BIND_SRV_VARIANCE)
    if (!DenoiserIsInside(coordinate.xy, DenoiserExtent(g_Denoiser.r_variance)))
    {
        return 0.0;
    }
    return g_Denoiser.r_variance.Load(coordinate).x;
#else
    return 0.0f;
#endif
}

float3 FFX_DENOISER_LoadWorldSpaceNormal(int2 pixel_coordinate)
{
#if defined(DENOISER_BIND_SRV_INPUT_NORMAL)
    uint width;
    uint height;
    g_Textures[NonUniformIndex(g_PC.NormalTexture)].GetDimensions(width, height);
    float3 normal = float3(0.0);
    if (DenoiserIsInside(pixel_coordinate, uint2(width, height)))
    {
        normal = g_Textures[NonUniformIndex(g_PC.NormalTexture)].Load(int3(pixel_coordinate, 0)).xyz;
    }
    return normalize(normal);
#else
    return float3(0.0f);
#endif
}

#if defined (DENOISER_BIND_SRV_EXTRACTED_ROUGHNESS)
float FFX_DNSR_Reflections_LoadRoughness(int2 pixel_coordinate)
{
    if (!DenoiserIsInside(pixel_coordinate, DenoiserExtent(g_Denoiser.r_extracted_roughness)))
    {
        return 0.0;
    }
    float rawRoughness = (float)g_Denoiser.r_extracted_roughness.Load(int3(pixel_coordinate, 0));
    return rawRoughness;
}
#endif // #if defined (DENOISER_BIND_SRV_EXTRACTED_ROUGHNESS)

void StoreRadiance(int2 coordinate, float4 radiance)
{
#if defined (DENOISER_BIND_UAV_RADIANCE)
    uint width;
    uint height;
    g_Denoiser.rw_radiance.GetDimensions(width, height);
    if (DenoiserIsInside(coordinate, uint2(width, height)))
    {
        g_Denoiser.rw_radiance[coordinate] = radiance;
    }
#endif
}

void StoreVariance(int2 coordinate, float variance)
{
#if defined (DENOISER_BIND_UAV_VARIANCE)
    uint width;
    uint height;
    g_Denoiser.rw_variance.GetDimensions(width, height);
    if (DenoiserIsInside(coordinate, uint2(width, height)))
    {
        g_Denoiser.rw_variance[coordinate] = variance;
    }
#endif
}

void FFX_DNSR_Reflections_StorePrefilteredReflections(int2 pixel_coordinate, float3 radiance, float variance)
{
    StoreRadiance(pixel_coordinate, radiance.xyzz);
    StoreVariance(pixel_coordinate, variance.x);
}

void FFX_DNSR_Reflections_StoreTemporalAccumulation(int2 pixel_coordinate, float3 radiance, float variance)
{
    StoreRadiance(pixel_coordinate, radiance.xyzz);
    StoreVariance(pixel_coordinate, variance.x);
}

#if defined (DENOISER_BIND_SRV_AVERAGE_RADIANCE)
float3 FFX_DNSR_Reflections_SampleAverageRadiance(float2 uv)
{
    return (float3)g_Denoiser.r_average_radiance.SampleLevel(g_Denoiser.s_LinearSampler, uv, 0.0f).xyz;
}
#endif // #if defined (DENOISER_BIND_SRV_AVERAGE_RADIANCE)

#if defined (DENOISER_BIND_SRV_RADIANCE_HISTORY)
float3 FFX_DNSR_Reflections_LoadRadianceHistory(int2 pixel_coordinate)
{
    if (!DenoiserIsInside(pixel_coordinate, DenoiserExtent(g_Denoiser.r_radiance_history)))
    {
        return float3(0.0);
    }
    return (float3)g_Denoiser.r_radiance_history.Load(int3(pixel_coordinate, 0)).xyz;
}
#endif // #if defined (DENOISER_BIND_SRV_RADIANCE_HISTORY)

#if defined (DENOISER_BIND_SRV_RADIANCE_HISTORY)
float3 FFX_DNSR_Reflections_SampleRadianceHistory(float2 uv)
{
    return (float3)g_Denoiser.r_radiance_history.SampleLevel(g_Denoiser.s_LinearSampler, uv, 0.0f).xyz;
}
#endif // #if defined (DENOISER_BIND_SRV_RADIANCE_HISTORY)

#if defined (DENOISER_BIND_SRV_VARIANCE)
float FFX_DNSR_Reflections_SampleVarianceHistory(float2 uv)
{
    return (float)g_Denoiser.r_variance.SampleLevel(g_Denoiser.s_LinearSampler, uv, 0.0f).x;
}
#endif // #if defined (DENOISER_BIND_SRV_VARIANCE)

#if defined (DENOISER_BIND_SRV_SAMPLE_COUNT)
float FFX_DNSR_Reflections_SampleNumSamplesHistory(float2 uv)
{
    return (float)g_Denoiser.r_sample_count.SampleLevel(g_Denoiser.s_LinearSampler, uv, 0.0f).x;
}
#endif // #if defined (DENOISER_BIND_SRV_SAMPLE_COUNT)

#if defined (DENOISER_BIND_UAV_REPROJECTED_RADIANCE)
void FFX_DNSR_Reflections_StoreRadianceReprojected(int2 pixel_coordinate, float3 value)
{
    uint width;
    uint height;
    g_Denoiser.rw_reprojected_radiance.GetDimensions(width, height);
    if (DenoiserIsInside(pixel_coordinate, uint2(width, height)))
    {
        g_Denoiser.rw_reprojected_radiance[pixel_coordinate] = float4(value, 0.0);
    }
}
#endif // #if defined (DENOISER_BIND_UAV_REPROJECTED_RADIANCE)

#if defined (DENOISER_BIND_UAV_AVERAGE_RADIANCE)
void FFX_DNSR_Reflections_StoreAverageRadiance(int2 pixel_coordinate, float3 value)
{
    uint width;
    uint height;
    g_Denoiser.rw_average_radiance.GetDimensions(width, height);
    if (DenoiserIsInside(pixel_coordinate, uint2(width, height)))
    {
        g_Denoiser.rw_average_radiance[pixel_coordinate] = value;
    }
}
#endif // #if defined (DENOISER_BIND_UAV_AVERAGE_RADIANCE)

float3 FFX_DNSR_Reflections_LoadWorldSpaceNormal(int2 pixel_coordinate)
{
    return FFX_DENOISER_LoadWorldSpaceNormal(pixel_coordinate);
}

#if defined (DENOISER_BIND_SRV_ROUGHNESS_HISTORY)
float FFX_DNSR_Reflections_SampleRoughnessHistory(float2 uv)
{
    float rawRoughness = (float)g_Denoiser.r_roughness_history.SampleLevel(g_Denoiser.s_LinearSampler, uv, 0.0f);
    return rawRoughness;
}
#endif // #if defined (DENOISER_BIND_SRV_ROUGHNESS_HISTORY)

#if defined (DENOISER_BIND_SRV_NORMAL_HISTORY)
float3 FFX_DNSR_Reflections_LoadWorldSpaceNormalHistory(int2 pixel_coordinate)
{
    float3 normal = float3(0.0);
    if (DenoiserIsInside(pixel_coordinate, DenoiserExtent(g_Denoiser.r_normal_history)))
    {
        normal = g_Denoiser.r_normal_history.Load(int3(pixel_coordinate, 0)).xyz;
    }
    return normalize((float3)(normal));
}
#endif // #if defined (DENOISER_BIND_SRV_NORMAL_HISTORY)

#if defined (DENOISER_BIND_SRV_NORMAL_HISTORY)
float3 FFX_DNSR_Reflections_SampleWorldSpaceNormalHistory(float2 uv)
{
    return normalize((float3)(g_Denoiser.r_normal_history.SampleLevel(g_Denoiser.s_LinearSampler, uv, 0.0f).xyz));
}
#endif // #if defined (DENOISER_BIND_SRV_NORMAL_HISTORY)

#if defined (DENOISER_BIND_SRV_RADIANCE)
float FFX_DNSR_Reflections_LoadRayLength(int2 pixel_coordinate)
{
    if (!DenoiserIsInside(pixel_coordinate, DenoiserExtent(g_Denoiser.r_radiance)))
    {
        return 0.0;
    }
    return (float)g_Denoiser.r_radiance.Load(int3(pixel_coordinate, 0)).w;
}
#endif // #if defined (DENOISER_BIND_SRV_RADIANCE)

void FFX_DNSR_Reflections_StoreVariance(int2 pixel_coordinate, float value)
{
    StoreVariance(pixel_coordinate, value);
}

#if defined (DENOISER_BIND_UAV_SAMPLE_COUNT)
void FFX_DNSR_Reflections_StoreNumSamples(int2 pixel_coordinate, float value)
{
    uint width;
    uint height;
    g_Denoiser.rw_sample_count.GetDimensions(width, height);
    if (DenoiserIsInside(pixel_coordinate, uint2(width, height)))
    {
        g_Denoiser.rw_sample_count[pixel_coordinate] = value;
    }
}
#endif // #if defined (DENOISER_BIND_UAV_SAMPLE_COUNT)

float3 FFX_DNSR_Reflections_LoadRadiance(int2 pixel_coordinate)
{
    return LoadRadiance(int3(pixel_coordinate, 0));
}

#if defined (DENOISER_BIND_SRV_REPROJECTED_RADIANCE)
float3 FFX_DNSR_Reflections_LoadRadianceReprojected(int2 pixel_coordinate)
{
    if (!DenoiserIsInside(pixel_coordinate, DenoiserExtent(g_Denoiser.r_reprojected_radiance)))
    {
        return float3(0.0);
    }
    return (float3)g_Denoiser.r_reprojected_radiance.Load(int3(pixel_coordinate, 0)).xyz;
}
#endif // #if defined (DENOISER_BIND_SRV_REPROJECTED_RADIANCE)

float FFX_DNSR_Reflections_LoadVariance(int2 pixel_coordinate)
{
    return LoadVariance(int3(pixel_coordinate, 0));
}

#if defined (DENOISER_BIND_SRV_SAMPLE_COUNT)
float FFX_DNSR_Reflections_LoadNumSamples(int2 pixel_coordinate)
{
    if (!DenoiserIsInside(pixel_coordinate, DenoiserExtent(g_Denoiser.r_sample_count)))
    {
        return 0.0;
    }
    return (float)g_Denoiser.r_sample_count.Load(int3(pixel_coordinate, 0)).x;
}
#endif // #if defined (DENOISER_BIND_SRV_SAMPLE_COUNT)

float FFX_DENOISER_LoadDepth(int2 pixel_coordinate, int mip)
{
#if defined(DENOISER_BIND_SRV_INPUT_DEPTH_HIERARCHY)
    uint width;
    uint height;
    uint levels;
    g_Denoiser.r_input_depth_hierarchy.GetDimensions(0, width, height, levels);
    if (uint(mip) >= levels || !DenoiserIsInside(pixel_coordinate, max(uint2(width, height) >> uint(mip), uint2(1))))
    {
        return 0.0;
    }
    return g_Denoiser.r_input_depth_hierarchy.Load(int3(pixel_coordinate, mip));
#else
    return 0.0f;
#endif
}

#if defined (DENOISER_BIND_UAV_DENOISER_TILE_LIST)
uint GetDenoiserTile(uint group_id)
{
    return g_Denoiser.rw_denoiser_tile_list[group_id];
}
#endif // #if defined (DENOISER_BIND_UAV_DENOISER_TILE_LIST)

#if defined (DENOISER_BIND_SRV_INPUT_MOTION_VECTORS)
// D4: the engine's motion vector is curUV - prevUV; the reference's is prevUV - curUV.
float2 MotionVectorScale()
{
    return float2(-1.0, -1.0);
}

float2 FFX_DNSR_Reflections_LoadMotionVector(int2 pixel_coordinate)
{
    uint width;
    uint height;
    g_Textures[NonUniformIndex(g_PC.MotionVectorTexture)].GetDimensions(width, height);
    if (!DenoiserIsInside(pixel_coordinate, uint2(width, height)))
    {
        return MotionVectorScale() * float2(0.0);
    }
    return MotionVectorScale() * g_Textures[NonUniformIndex(g_PC.MotionVectorTexture)].Load(int3(pixel_coordinate, 0)).xy;
}
#endif // #if defined (DENOISER_BIND_SRV_INPUT_MOTION_VECTORS)

float FFX_DNSR_Reflections_LoadDepth(int2 pixel_coordinate)
{
    return FFX_DENOISER_LoadDepth(pixel_coordinate, 0);
}

#if defined (DENOISER_BIND_SRV_DEPTH_HISTORY)
float FFX_DNSR_Reflections_LoadDepthHistory(int2 pixel_coordinate)
{
    if (!DenoiserIsInside(pixel_coordinate, DenoiserExtent(g_Denoiser.r_depth_history)))
    {
        return 0.0;
    }
    return g_Denoiser.r_depth_history.Load(int3(pixel_coordinate, 0));
}
#endif // #if defined (DENOISER_BIND_SRV_DEPTH_HISTORY)

#if defined (DENOISER_BIND_SRV_DEPTH_HISTORY)
float FFX_DNSR_Reflections_SampleDepthHistory(float2 uv)
{
    return g_Denoiser.r_depth_history.SampleLevel(g_Denoiser.s_LinearSampler, uv, 0.0f);
}
#endif // #if defined (DENOISER_BIND_SRV_DEPTH_HISTORY)
