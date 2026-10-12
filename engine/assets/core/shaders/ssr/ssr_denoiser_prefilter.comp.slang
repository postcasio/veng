// Screen-space reflections: the denoiser's spatial prefilter. Ports
// ffx_denoiser_reflections_prefilter.h (its 32-bit path) and its pass entry point
// (ffx_denoiser_prefilter_reflections_pass.hlsl) from the FidelityFX SDK v1.1.4.
//
// One 8x8 workgroup per listed denoiser tile, dispatched indirectly. A glossy, non-mirror pixel
// with variance above zero is resolved from itself and 15 Halton(2, 3) taps within 3 pixels,
// weighted by normal and depth edge-stopping, by each tap's distance from the tile's 8x8 average
// radiance and by the pixel's variance; every other pixel copies itself through.
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

#define DENOISER_BIND_SRV_INPUT_DEPTH_HIERARCHY    2
#define DENOISER_BIND_SRV_INPUT_NORMAL             1
#define DENOISER_BIND_SRV_RADIANCE                 3
#define DENOISER_BIND_SRV_VARIANCE                 4
#define DENOISER_BIND_SRV_AVERAGE_RADIANCE         5
#define DENOISER_BIND_SRV_EXTRACTED_ROUGHNESS      6

#define DENOISER_BIND_UAV_RADIANCE                 7
#define DENOISER_BIND_UAV_VARIANCE                 8
#define DENOISER_BIND_UAV_DENOISER_TILE_LIST       9

#include "ssr_denoiser_callbacks.slang"

groupshared uint  g_ffx_dnsr_shared_0[16][16];
groupshared uint  g_ffx_dnsr_shared_1[16][16];
groupshared uint  g_ffx_dnsr_shared_2[16][16];
groupshared uint  g_ffx_dnsr_shared_3[16][16];
groupshared float g_ffx_dnsr_shared_depth[16][16];

struct FFX_DNSR_Reflections_NeighborhoodSample {
    float3    radiance;
    float      variance;
    float3    normal;
    float      depth;
};

FFX_DNSR_Reflections_NeighborhoodSample FFX_DNSR_Reflections_LoadFromGroupSharedMemory(int2 idx) {
    uint2     packed_radiance          = uint2(g_ffx_dnsr_shared_0[idx.y][idx.x], g_ffx_dnsr_shared_1[idx.y][idx.x]);
    float4    unpacked_radiance        = FFX_DNSR_Reflections_UnpackFloat16_4(packed_radiance);
    uint2     packed_normal_variance   = uint2(g_ffx_dnsr_shared_2[idx.y][idx.x], g_ffx_dnsr_shared_3[idx.y][idx.x]);
    float4    unpacked_normal_variance = FFX_DNSR_Reflections_UnpackFloat16_4(packed_normal_variance);

    FFX_DNSR_Reflections_NeighborhoodSample neighborSample;
    neighborSample.radiance = unpacked_radiance.xyz;
    neighborSample.normal   = unpacked_normal_variance.xyz;
    neighborSample.variance = unpacked_normal_variance.w;
    neighborSample.depth    = g_ffx_dnsr_shared_depth[idx.y][idx.x];
    return neighborSample;
}

void FFX_DNSR_Reflections_StoreInGroupSharedMemory(int2 group_thread_id, float3 radiance, float variance, float3 normal, float depth) {
    g_ffx_dnsr_shared_0[group_thread_id.y][group_thread_id.x]     = FFX_DNSR_Reflections_PackFloat16(radiance.xy);
    g_ffx_dnsr_shared_1[group_thread_id.y][group_thread_id.x]     = FFX_DNSR_Reflections_PackFloat16(radiance.zz);
    g_ffx_dnsr_shared_2[group_thread_id.y][group_thread_id.x]     = FFX_DNSR_Reflections_PackFloat16(normal.xy);
    g_ffx_dnsr_shared_3[group_thread_id.y][group_thread_id.x]     = FFX_DNSR_Reflections_PackFloat16(float2(normal.z, variance));
    g_ffx_dnsr_shared_depth[group_thread_id.y][group_thread_id.x] = depth;
}

void FFX_DNSR_Reflections_LoadNeighborhood(
    int2 pixel_coordinate,
    out float3 radiance,
    out float variance,
    out float3 normal,
    out float depth,
    int2 screen_size) {

    radiance = LoadRadiance(int3(pixel_coordinate, 0));
    variance = LoadVariance(int3(pixel_coordinate, 0));

    normal = FFX_DENOISER_LoadWorldSpaceNormal(pixel_coordinate);

    float2 uv = (pixel_coordinate.xy + (0.5f).xx) / float2(screen_size.xy);
    depth = FFX_DNSR_Reflections_GetLinearDepth(uv, FFX_DENOISER_LoadDepth(pixel_coordinate, 0));
}

void FFX_DNSR_Reflections_InitializeGroupSharedMemory(int2 dispatch_thread_id, int2 group_thread_id, int2 screen_size) {
    // Load 16x16 region into shared memory using 4 8x8 blocks.
    int2 offset[4] = {int2(0, 0), int2(8, 0), int2(0, 8), int2(8, 8)};

    // Intermediate storage registers to cache the result of all loads
    float3 radiance[4];
    float  variance[4];
    float3 normal[4];
    float       depth[4];

    // Start in the upper left corner of the 16x16 region.
    dispatch_thread_id -= 4;

    // First store all loads in registers
    for (int i = 0; i < 4; ++i) {
        FFX_DNSR_Reflections_LoadNeighborhood(dispatch_thread_id + offset[i], radiance[i], variance[i], normal[i], depth[i], screen_size);
    }

    // Then move all registers to groupshared memory
    for (int j = 0; j < 4; ++j) {
        FFX_DNSR_Reflections_StoreInGroupSharedMemory(group_thread_id + offset[j], radiance[j], variance[j], normal[j], depth[j]); // X
    }
}

float FFX_DNSR_Reflections_GetEdgeStoppingNormalWeight(float3 normal_p, float3 normal_q) {
    return float(pow(max(dot(normal_p, normal_q), float(0.0f)), FFX_DNSR_REFLECTIONS_PREFILTER_NORMAL_SIGMA));
}

float FFX_DNSR_Reflections_GetEdgeStoppingDepthWeight(float center_depth, float neighbor_depth) {
    return float(exp(-abs(center_depth - neighbor_depth) * center_depth * FFX_DNSR_REFLECTIONS_PREFILTER_DEPTH_SIGMA));
}

float FFX_DNSR_Reflections_GetRadianceWeight(float3 center_radiance, float3 neighbor_radiance, float variance) {
    return float(max(exp(-(FFX_DNSR_REFLECTIONS_RADIANCE_WEIGHT_BIAS + variance * FFX_DNSR_REFLECTIONS_RADIANCE_WEIGHT_VARIANCE_K)
                    * length(center_radiance - neighbor_radiance.xyz))
            , 1.0e-2));
}

void FFX_DNSR_Reflections_Resolve(int2 group_thread_id, float3 avg_radiance, FFX_DNSR_Reflections_NeighborhoodSample center,
                                  out float3 resolved_radiance, out float resolved_variance) {
    // Initial weight is important to remove fireflies.
    // That removes quite a bit of energy but makes everything much more stable.
    float  accumulated_weight   = FFX_DNSR_Reflections_GetRadianceWeight(avg_radiance, center.radiance.xyz, center.variance);
    float3 accumulated_radiance = center.radiance.xyz * accumulated_weight;
    float  accumulated_variance = center.variance * accumulated_weight * accumulated_weight;
    // First 15 numbers of Halton(2,3) streteched to [-3,3]. Skipping the center, as we already have that in center_radiance and center_variance.
    const uint sample_count     = 15;
    const int2 sample_offsets[] = {int2(0, 1),  int2(-2, 1),  int2(2, -3), int2(-3, 0),  int2(1, 2), int2(-1, -2), int2(3, 0), int2(-3, 3),
                                   int2(0, -3), int2(-1, -1), int2(2, 1),  int2(-2, -2), int2(1, 0), int2(0, 2),   int2(3, -1)};
    float variance_weight = float(max(FFX_DNSR_REFLECTIONS_PREFILTER_VARIANCE_BIAS,
                                     1.0 - exp(-(center.variance * FFX_DNSR_REFLECTIONS_PREFILTER_VARIANCE_WEIGHT))
                                    ));
    for (int i = 0; i < sample_count; ++i) {
        int2                                    new_idx  = group_thread_id + sample_offsets[i];
        FFX_DNSR_Reflections_NeighborhoodSample neighbor = FFX_DNSR_Reflections_LoadFromGroupSharedMemory(new_idx);

        float weight = float(1.0f);
        weight *= FFX_DNSR_Reflections_GetEdgeStoppingNormalWeight(center.normal, neighbor.normal);
        weight *= FFX_DNSR_Reflections_GetEdgeStoppingDepthWeight(center.depth, neighbor.depth);
        weight *= FFX_DNSR_Reflections_GetRadianceWeight(avg_radiance, neighbor.radiance.xyz, center.variance);
        weight *= variance_weight;

        // Accumulate all contributions.
        accumulated_weight += weight;
        accumulated_radiance += weight * neighbor.radiance.xyz;
        accumulated_variance += weight * weight * neighbor.variance;
    }

    accumulated_radiance /= accumulated_weight;
    accumulated_variance /= (accumulated_weight * accumulated_weight);
    resolved_radiance = accumulated_radiance;
    resolved_variance = accumulated_variance;
}

void FFX_DNSR_Reflections_Prefilter(int2 dispatch_thread_id, int2 group_thread_id, uint2 screen_size) {
    float center_roughness = FFX_DNSR_Reflections_LoadRoughness(dispatch_thread_id);
    FFX_DNSR_Reflections_InitializeGroupSharedMemory(dispatch_thread_id, group_thread_id, int2(screen_size));
    GroupMemoryBarrierWithGroupSync();

    group_thread_id += 4; // Center threads in groupshared memory

    FFX_DNSR_Reflections_NeighborhoodSample center = FFX_DNSR_Reflections_LoadFromGroupSharedMemory(group_thread_id);

    float3 resolved_radiance = center.radiance;
    float  resolved_variance = center.variance;

    // Check if we have to denoise or if a simple copy is enough
    bool needs_denoiser = center.variance > 0.0 && FFX_DNSR_Reflections_IsGlossyReflection(center_roughness) && !FFX_DNSR_Reflections_IsMirrorReflection(center_roughness);
    if (needs_denoiser) {
        float2      uv8          = (float2(dispatch_thread_id.xy) + (0.5).xx) / float2(FFX_DNSR_Reflections_RoundUp8(screen_size));
        float3 avg_radiance = FFX_DNSR_Reflections_SampleAverageRadiance(uv8);
        FFX_DNSR_Reflections_Resolve(group_thread_id, avg_radiance, center, resolved_radiance, resolved_variance);
    }

    FFX_DNSR_Reflections_StorePrefilteredReflections(dispatch_thread_id, resolved_radiance, resolved_variance);
}

void Prefilter(uint group_index, uint group_id, int2 group_thread_id) {
    uint   packed_coords = GetDenoiserTile(group_id);
    int2  dispatch_thread_id = int2(packed_coords & 0xffffu, (packed_coords >> 16) & 0xffffu) + group_thread_id;
    int2  dispatch_group_id = dispatch_thread_id / 8;
    int2 remapped_group_thread_id = int2(DenoiserRemapForWaveReduction(group_index));
    int2 remapped_dispatch_thread_id = int2(dispatch_group_id * 8 + remapped_group_thread_id);

    FFX_DNSR_Reflections_Prefilter(remapped_dispatch_thread_id, remapped_group_thread_id, DenoiserRenderSize());
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint LocalThreadIndex : SV_GroupIndex, uint3 WorkGroupId : SV_GroupID, uint3 GroupThreadId : SV_GroupThreadID)
{
    Prefilter(LocalThreadIndex, WorkGroupId.x, int2(GroupThreadId.xy));
}
