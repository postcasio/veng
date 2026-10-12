// Screen-space reflections: the denoiser's temporal resolve, which writes the denoised reflection.
// Ports ffx_denoiser_reflections_resolve_temporal.h (its 32-bit path) and its pass entry point
// (ffx_denoiser_resolve_temporal_reflections_pass.hlsl) from the FidelityFX SDK v1.1.4.
//
// One 8x8 workgroup per listed denoiser tile, dispatched indirectly. A pixel whose prefiltered
// radiance sums under 1e-4 resolves to zero. A glossy pixel clips its reprojected history into a
// box about the 9x9 statistics of the prefiltered radiance (Playdead's ClipAABB), blends its new
// signal towards the tile's 8x8 average by its sample count and clips it about that average, then
// blends the two by the sample count; a non-finite result resolves to zero. Every other pixel keeps
// its prefiltered radiance and variance.
//
// The reference's pass binds the radiance history as well, which nothing in it reads; it is left
// unbound here.
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

#define DENOISER_BIND_SRV_RADIANCE                 2
#define DENOISER_BIND_SRV_VARIANCE                 3
#define DENOISER_BIND_SRV_SAMPLE_COUNT             4
#define DENOISER_BIND_SRV_AVERAGE_RADIANCE         5
#define DENOISER_BIND_SRV_EXTRACTED_ROUGHNESS      6
#define DENOISER_BIND_SRV_REPROJECTED_RADIANCE     7

#define DENOISER_BIND_UAV_RADIANCE                 8
#define DENOISER_BIND_UAV_VARIANCE                 9
#define DENOISER_BIND_UAV_DENOISER_TILE_LIST       10

#define RADIANCE_THRESHOLD 0.0001

#define FFX_DNSR_REFLECTIONS_ESTIMATES_LOCAL_NEIGHBORHOOD
#include "ssr_denoiser_callbacks.slang"

groupshared uint g_ffx_dnsr_shared_0[16][16];
groupshared uint g_ffx_dnsr_shared_1[16][16];

struct FFX_DNSR_Reflections_NeighborhoodSample {
    float3 radiance;
};

FFX_DNSR_Reflections_NeighborhoodSample FFX_DNSR_Reflections_LoadFromGroupSharedMemory(int2 idx) {
    uint2  packed_radiance   = uint2(g_ffx_dnsr_shared_0[idx.y][idx.x], g_ffx_dnsr_shared_1[idx.y][idx.x]);
    float3 unpacked_radiance = FFX_DNSR_Reflections_UnpackFloat16_4(packed_radiance).xyz;

    FFX_DNSR_Reflections_NeighborhoodSample neighborSample;
    neighborSample.radiance = unpacked_radiance;
    return neighborSample;
}

struct FFX_DNSR_Reflections_Moments {
    float3 mean;
    float3 variance;
};

FFX_DNSR_Reflections_Moments FFX_DNSR_Reflections_EstimateLocalNeighborhoodInGroup(int2 group_thread_id) {
    FFX_DNSR_Reflections_Moments estimate;
    estimate.mean                 = float3(0.0f, 0.0f, 0.0f);
    estimate.variance             = float3(0.0f, 0.0f, 0.0f);
    float accumulated_weight = float(0.0f);
    for (int j = -FFX_DNSR_REFLECTIONS_LOCAL_NEIGHBORHOOD_RADIUS; j <= FFX_DNSR_REFLECTIONS_LOCAL_NEIGHBORHOOD_RADIUS; ++j) {
        for (int i = -FFX_DNSR_REFLECTIONS_LOCAL_NEIGHBORHOOD_RADIUS; i <= FFX_DNSR_REFLECTIONS_LOCAL_NEIGHBORHOOD_RADIUS; ++i) {
            int2        new_idx  = group_thread_id + int2(i, j);
            float3 radiance = FFX_DNSR_Reflections_LoadFromGroupSharedMemory(new_idx).radiance;
            float   weight   = FFX_DNSR_Reflections_LocalNeighborhoodKernelWeight(float(i)) * FFX_DNSR_Reflections_LocalNeighborhoodKernelWeight(float(j));
            accumulated_weight  += weight;
            estimate.mean       += radiance * weight;
            estimate.variance   += radiance * radiance * weight;
        }
    }
    estimate.mean     /= accumulated_weight;
    estimate.variance /= accumulated_weight;

    estimate.variance = abs(estimate.variance - estimate.mean * estimate.mean);
    return estimate;
}

void FFX_DNSR_Reflections_StoreInGroupSharedMemory(int2 group_thread_id, float3 radiance) {
    g_ffx_dnsr_shared_0[group_thread_id.y][group_thread_id.x] = FFX_DNSR_Reflections_PackFloat16(radiance.xy);
    g_ffx_dnsr_shared_1[group_thread_id.y][group_thread_id.x] = FFX_DNSR_Reflections_PackFloat16(radiance.zz);
}

void FFX_DNSR_Reflections_LoadNeighborhood(int2 pixel_coordinate, out float3 radiance) { radiance = FFX_DNSR_Reflections_LoadRadiance(pixel_coordinate); }

void FFX_DNSR_Reflections_InitializeGroupSharedMemory(int2 dispatch_thread_id, int2 group_thread_id, int2 screen_size) {
    // Load 16x16 region into shared memory using 4 8x8 blocks.
    int2 offset[4] = {int2(0, 0), int2(8, 0), int2(0, 8), int2(8, 8)};

    // Intermediate storage registers to cache the result of all loads
    float3 radiance[4];

    // Start in the upper left corner of the 16x16 region.
    dispatch_thread_id -= 4;

    // First store all loads in registers
    for (int i = 0; i < 4; ++i) {
        FFX_DNSR_Reflections_LoadNeighborhood(dispatch_thread_id + offset[i], radiance[i]);
    }

    // Then move all registers to FFX_GROUPSHARED memory
    for (int j = 0; j < 4; ++j) {
        FFX_DNSR_Reflections_StoreInGroupSharedMemory(group_thread_id + offset[j], radiance[j]);
    }
}

void FFX_DNSR_Reflections_ResolveTemporal(int2 dispatch_thread_id, int2 group_thread_id, uint2 screen_size, float2 inv_screen_size, float history_clip_weight) {
    FFX_DNSR_Reflections_InitializeGroupSharedMemory(dispatch_thread_id, group_thread_id, int2(screen_size));
    GroupMemoryBarrierWithGroupSync();

    group_thread_id += 4; // Center threads in FFX_GROUPSHARED memory

    float3 radiance = FFX_DNSR_Reflections_LoadFromGroupSharedMemory(group_thread_id).radiance;
    float radianceSum = radiance.x + radiance.y + radiance.z;
    if (radianceSum < RADIANCE_THRESHOLD)
    {
        FFX_DNSR_Reflections_StoreTemporalAccumulation(dispatch_thread_id, float3(0.0f, 0.0f, 0.0f), float(0.0f));
        return;
    }

    FFX_DNSR_Reflections_NeighborhoodSample center       = FFX_DNSR_Reflections_LoadFromGroupSharedMemory(group_thread_id);
    float3                                  new_signal   = center.radiance;
    float                                   roughness    = FFX_DNSR_Reflections_LoadRoughness(dispatch_thread_id);
    float                                   new_variance = FFX_DNSR_Reflections_LoadVariance(dispatch_thread_id);

    if (FFX_DNSR_Reflections_IsGlossyReflection(roughness)) {
        float      num_samples  = FFX_DNSR_Reflections_LoadNumSamples(dispatch_thread_id);
        float2    uv8          = (float2(dispatch_thread_id.xy) + (0.5).xx) / float2(FFX_DNSR_Reflections_RoundUp8(screen_size));
        float3    avg_radiance = FFX_DNSR_Reflections_SampleAverageRadiance(uv8);

        float3                  old_signal         = FFX_DNSR_Reflections_LoadRadianceReprojected(dispatch_thread_id);
        FFX_DNSR_Reflections_Moments local_neighborhood  = FFX_DNSR_Reflections_EstimateLocalNeighborhoodInGroup(group_thread_id);
        // Clip history based on the curren local statistics
        float3                  color_std          = float3((sqrt(local_neighborhood.variance.xyz) + length(local_neighborhood.mean.xyz - avg_radiance)) * history_clip_weight * 1.4f);
                             local_neighborhood.mean.xyz = lerp(local_neighborhood.mean.xyz, avg_radiance, float3(0.2f, 0.2f, 0.2f));
        float3                  radiance_min       = local_neighborhood.mean.xyz - color_std;
        float3                  radiance_max       = local_neighborhood.mean.xyz + color_std;
        float3                  clipped_old_signal = FFX_DNSR_Reflections_ClipAABB(radiance_min, radiance_max, old_signal.xyz);
        float                    accumulation_speed = float(1.0f) / max(num_samples, float(1.0f));
        float                    weight             = (float(1.0f) - accumulation_speed);
        // Blend with average for small sample count
        new_signal.xyz                                   = lerp(new_signal.xyz, avg_radiance, float(1.0f) / max(num_samples + float(1.0f), float(1.0f)));
        // Clip outliers
        {
            float3                  radiance_min   = avg_radiance.xyz - color_std * float3(1.0f, 1.0f, 1.0f);
            float3                  radiance_max   = avg_radiance.xyz + color_std * float3(1.0f, 1.0f, 1.0f);
            new_signal.xyz                               = FFX_DNSR_Reflections_ClipAABB(radiance_min, radiance_max, new_signal.xyz);
        }
        // Blend with history
        new_signal                                       = lerp(new_signal, clipped_old_signal, weight);
        new_variance                                     = lerp(FFX_DNSR_Reflections_ComputeTemporalVariance(new_signal.xyz, clipped_old_signal.xyz), new_variance, weight);
        if (!DenoiserIsFinite(new_signal) || !DenoiserIsFinite(float3(new_variance, 0.0, 0.0))) {
            new_signal   = float3(0.0f, 0.0f, 0.0f);
            new_variance = float(0.0f);
        }

    }
    FFX_DNSR_Reflections_StoreTemporalAccumulation(dispatch_thread_id, new_signal, new_variance);
}

void ResolveTemporal(uint group_index, uint group_id, uint2 group_thread_id) {
    uint  packed_coords = GetDenoiserTile(group_id);
    int2  dispatch_thread_id = int2(packed_coords & 0xffffu, (packed_coords >> 16) & 0xffffu) + int2(group_thread_id);
    int2  dispatch_group_id = dispatch_thread_id / 8;
    int2 remapped_group_thread_id = int2(DenoiserRemapForWaveReduction(group_index));
    int2 remapped_dispatch_thread_id = dispatch_group_id * 8 + remapped_group_thread_id;

    FFX_DNSR_Reflections_ResolveTemporal(remapped_dispatch_thread_id, remapped_group_thread_id, DenoiserRenderSize(), DenoiserInverseRenderSize(), TemporalStabilityFactor());
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint LocalThreadIndex : SV_GroupIndex, uint3 WorkGroupId : SV_GroupID, uint3 GroupThreadId : SV_GroupThreadID)
{
    ResolveTemporal(LocalThreadIndex, WorkGroupId.x, GroupThreadId.xy);
}
