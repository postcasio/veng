// Screen-space reflections: the denoiser's reprojection. Ports ffx_denoiser_reflections_reproject.h
// (its 32-bit path) and its pass entry point (ffx_denoiser_reproject_reflections_pass.hlsl) from the
// FidelityFX SDK v1.1.4.
//
// One 8x8 workgroup per listed denoiser tile, dispatched indirectly. Per glossy pixel it picks the
// history to reproject from: the virtual hit point's (the parallax candidate) when its normal and
// roughness match better, else the surface's when it is close to the 9x9 Gaussian statistics of
// this frame's radiance, else none. It weighs the pick's disocclusion, widening the search to a
// 3x3 neighbourhood and then a 2x2 bilinear footprint below the threshold, and writes the
// reprojected radiance, the temporal variance and the sample count. The workgroup also reduces its
// tile's luminance-weighted radiance to the tile's 8x8 average.
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
#define DENOISER_BIND_SRV_INPUT_MOTION_VECTORS     1
#define DENOISER_BIND_SRV_INPUT_NORMAL             1
#define DENOISER_BIND_SRV_RADIANCE                 3
#define DENOISER_BIND_SRV_RADIANCE_HISTORY         4
#define DENOISER_BIND_SRV_VARIANCE                 5
#define DENOISER_BIND_SRV_SAMPLE_COUNT             6
#define DENOISER_BIND_SRV_EXTRACTED_ROUGHNESS      7
#define DENOISER_BIND_SRV_DEPTH_HISTORY            8
#define DENOISER_BIND_SRV_NORMAL_HISTORY           9
#define DENOISER_BIND_SRV_ROUGHNESS_HISTORY        10

#define DENOISER_BIND_UAV_VARIANCE                 11
#define DENOISER_BIND_UAV_SAMPLE_COUNT             12
#define DENOISER_BIND_UAV_AVERAGE_RADIANCE         13
#define DENOISER_BIND_UAV_DENOISER_TILE_LIST       14
#define DENOISER_BIND_UAV_REPROJECTED_RADIANCE     15

#define FFX_DNSR_REFLECTIONS_ESTIMATES_LOCAL_NEIGHBORHOOD
#include "ssr_denoiser_callbacks.slang"

groupshared uint g_ffx_dnsr_shared_0[16][16];
groupshared uint g_ffx_dnsr_shared_1[16][16];

struct FFX_DNSR_Reflections_NeighborhoodSample {
    float3 radiance;
};

FFX_DNSR_Reflections_NeighborhoodSample FFX_DNSR_Reflections_LoadFromGroupSharedMemory(int2 idx) {
    uint2  packed_radiance          = uint2(g_ffx_dnsr_shared_0[idx.y][idx.x], g_ffx_dnsr_shared_1[idx.y][idx.x]);
    float4 unpacked_radiance        = FFX_DNSR_Reflections_UnpackFloat16_4(packed_radiance);

    FFX_DNSR_Reflections_NeighborhoodSample neighborSample;
    neighborSample.radiance = unpacked_radiance.xyz;
    return neighborSample;
}

void FFX_DNSR_Reflections_StoreInGroupSharedMemory(int2 group_thread_id, float3 radiance) {
    g_ffx_dnsr_shared_0[group_thread_id.y][group_thread_id.x]     = FFX_DNSR_Reflections_PackFloat16(radiance.xy);
    g_ffx_dnsr_shared_1[group_thread_id.y][group_thread_id.x]     = FFX_DNSR_Reflections_PackFloat16(radiance.zz);
}

void FFX_DNSR_Reflections_StoreInGroupSharedMemory(int2 group_thread_id, float4 radiance_variance) {
    g_ffx_dnsr_shared_0[group_thread_id.y][group_thread_id.x]     = FFX_DNSR_Reflections_PackFloat16(radiance_variance.xy);
    g_ffx_dnsr_shared_1[group_thread_id.y][group_thread_id.x]     = FFX_DNSR_Reflections_PackFloat16(radiance_variance.zw);
}

void FFX_DNSR_Reflections_InitializeGroupSharedMemory(int2 dispatch_thread_id, int2 group_thread_id, int2 screen_size) {
    // Load 16x16 region into shared memory using 4 8x8 blocks.
    int2 offset[4] = {int2(0, 0), int2(8, 0), int2(0, 8), int2(8, 8)};

    // Intermediate storage registers to cache the result of all loads
    float3 radiance[4];

    // Start in the upper left corner of the 16x16 region.
    dispatch_thread_id -= 4;

    // First store all loads in registers
    for (int i = 0; i < 4; ++i) {
        radiance[i] = FFX_DNSR_Reflections_LoadRadiance(dispatch_thread_id + offset[i]);
    }

    // Then move all registers to FFX_GROUPSHARED memory
    for (int j = 0; j < 4; ++j) {
        FFX_DNSR_Reflections_StoreInGroupSharedMemory(group_thread_id + offset[j], radiance[j]); // X
    }
}

float4 FFX_DNSR_Reflections_LoadFromGroupSharedMemoryRaw(int2 idx) {
    uint2 packed_radiance = uint2(g_ffx_dnsr_shared_0[idx.y][idx.x], g_ffx_dnsr_shared_1[idx.y][idx.x]);
    return FFX_DNSR_Reflections_UnpackFloat16_4(packed_radiance);
}

float FFX_DNSR_Reflections_GetLuminanceWeight(float3 val) {
    float luma   = FFX_DNSR_Reflections_Luminance(val.xyz);
    float weight = float(max(exp(-luma * FFX_DNSR_REFLECTIONS_AVG_RADIANCE_LUMINANCE_WEIGHT), 1.0e-2));
    return weight;
}

float2 FFX_DNSR_Reflections_GetSurfaceReprojection(int2 dispatch_thread_id, float2 uv, float2 motion_vector) {
    // Reflector position reprojection
    float2 history_uv = uv + motion_vector;
    return history_uv;
}

float2 FFX_DNSR_Reflections_GetHitPositionReprojection(int2 dispatch_thread_id, float2 uv, float reflected_ray_length) {
    float  z               = FFX_DNSR_Reflections_LoadDepth(dispatch_thread_id);
    float3 view_space_ray = FFX_DNSR_Reflections_ScreenSpaceToViewSpace(float3(uv, z));

    // We start out with reconstructing the ray length in view space.
    // This includes the portion from the camera to the reflecting surface as well as the portion from the surface to the hit position.
    float surface_depth = length(view_space_ray);
    float ray_length    = surface_depth + reflected_ray_length;

    // We then perform a parallax correction by shooting a ray
    // of the same length "straight through" the reflecting surface
    // and reprojecting the tip of that ray to the previous frame.
    view_space_ray /= surface_depth; // == normalize(view_space_ray)
    view_space_ray *= ray_length;
    float3 world_hit_position =
        FFX_DNSR_Reflections_ViewSpaceToWorldSpace(float4(view_space_ray, 1)); // This is the "fake" hit position if we would follow the ray straight through the surface.
    float3 prev_hit_position = FFX_DNSR_Reflections_WorldSpaceToScreenSpacePrevious(world_hit_position);
    float2 history_uv        = prev_hit_position.xy;
    return history_uv;
}

float FFX_DNSR_Reflections_GetDisocclusionFactor(float3 normal, float3 history_normal, float linear_depth, float history_linear_depth) {
    float factor = float(1.0f                                                            //
                        * exp(-abs(1.0f - max(float(0.0f), dot(normal, history_normal))) * FFX_DNSR_REFLECTIONS_DISOCCLUSION_NORMAL_WEIGHT) //
                        * exp(-abs(history_linear_depth - linear_depth) / linear_depth * FFX_DNSR_REFLECTIONS_DISOCCLUSION_DEPTH_WEIGHT));
    return factor;
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
            float  weight   = FFX_DNSR_Reflections_LocalNeighborhoodKernelWeight(float(i)) * FFX_DNSR_Reflections_LocalNeighborhoodKernelWeight(float(j));
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

float dot2(float3 a) { return dot(a, a); }

void FFX_DNSR_Reflections_PickReprojection(int2       dispatch_thread_id,  //
                                           int2       group_thread_id,     //
                                           uint2      screen_size,         //
                                           float       roughness,           //
                                           float       ray_length,          //
                                           out float   disocclusion_factor, //
                                           out float2 reprojection_uv,     //
                                           out float3 reprojection) {

    // The reference leaves both unwritten on the early return below; every caller's path then
    // stores the same values whatever they hold.
    reprojection_uv = float2(0.0);
    reprojection    = float3(0.0);

    FFX_DNSR_Reflections_Moments local_neighborhood = FFX_DNSR_Reflections_EstimateLocalNeighborhoodInGroup(group_thread_id);

    float2 uv     = float2(dispatch_thread_id.x + 0.5, dispatch_thread_id.y + 0.5) / float2(screen_size);
    float3 normal = FFX_DNSR_Reflections_LoadWorldSpaceNormal(dispatch_thread_id);
    float3 history_normal;
    float   history_linear_depth;

    {
        const float2  motion_vector             = FFX_DNSR_Reflections_LoadMotionVector(dispatch_thread_id);
        const float2  surface_reprojection_uv   = FFX_DNSR_Reflections_GetSurfaceReprojection(dispatch_thread_id, uv, motion_vector);
        const float2  hit_reprojection_uv       = FFX_DNSR_Reflections_GetHitPositionReprojection(dispatch_thread_id, uv, ray_length);
        const float3  surface_normal            = FFX_DNSR_Reflections_SampleWorldSpaceNormalHistory(surface_reprojection_uv);
        const float3  hit_normal                = FFX_DNSR_Reflections_SampleWorldSpaceNormalHistory(hit_reprojection_uv);
        const float3  surface_history           = FFX_DNSR_Reflections_SampleRadianceHistory(surface_reprojection_uv);
        const float3  hit_history               = FFX_DNSR_Reflections_SampleRadianceHistory(hit_reprojection_uv);
        const float    hit_normal_similarity     = dot(normalize(float3(hit_normal)), normalize(float3(normal)));
        const float    surface_normal_similarity = dot(normalize(float3(surface_normal)), normalize(float3(normal)));
        const float    hit_roughness             = FFX_DNSR_Reflections_SampleRoughnessHistory(hit_reprojection_uv);
        const float    surface_roughness         = FFX_DNSR_Reflections_SampleRoughnessHistory(surface_reprojection_uv);

        // Choose reprojection uv based on similarity to the local neighborhood.
        if (hit_normal_similarity > FFX_DNSR_REFLECTIONS_REPROJECTION_NORMAL_SIMILARITY_THRESHOLD  // Candidate for mirror reflection parallax
            && hit_normal_similarity + 1.0e-3 > surface_normal_similarity                          //
            && abs(hit_roughness - roughness) < abs(surface_roughness - roughness) + 1.0e-3        //
        ) {
            history_normal                      = hit_normal;
            float hit_history_depth        = FFX_DNSR_Reflections_SampleDepthHistory(hit_reprojection_uv);
            float hit_history_linear_depth = FFX_DNSR_Reflections_GetLinearDepth(hit_reprojection_uv, hit_history_depth);
            history_linear_depth                = hit_history_linear_depth;
            reprojection_uv                     = hit_reprojection_uv;
            reprojection                        = hit_history;
        } else {
            // Reject surface reprojection based on simple distance
            if (dot2(surface_history - local_neighborhood.mean) <
                FFX_DNSR_REFLECTIONS_REPROJECT_SURFACE_DISCARD_VARIANCE_WEIGHT * length(local_neighborhood.variance)) {
                history_normal                          = surface_normal;
                float surface_history_depth        = FFX_DNSR_Reflections_SampleDepthHistory(surface_reprojection_uv);
                float surface_history_linear_depth = FFX_DNSR_Reflections_GetLinearDepth(surface_reprojection_uv, surface_history_depth);
                history_linear_depth                    = surface_history_linear_depth;
                reprojection_uv                         = surface_reprojection_uv;
                reprojection                            = surface_history;
            } else {
                disocclusion_factor = float(0.0f);
                return;
            }
        }
    }
    float depth        = FFX_DNSR_Reflections_LoadDepth(dispatch_thread_id);
    float linear_depth = FFX_DNSR_Reflections_GetLinearDepth(uv, depth);
    // Determine disocclusion factor based on history
    disocclusion_factor = FFX_DNSR_Reflections_GetDisocclusionFactor(normal, history_normal, linear_depth, history_linear_depth);

    if (disocclusion_factor > FFX_DNSR_REFLECTIONS_DISOCCLUSION_THRESHOLD) // Early out, good enough
        return;

    // Try to find the closest sample in the vicinity if we are not convinced of a disocclusion
    if (disocclusion_factor < FFX_DNSR_REFLECTIONS_DISOCCLUSION_THRESHOLD) {
        float2    closest_uv    = reprojection_uv;
        float2    dudv          = 1.0 / float2(screen_size);
        const int search_radius = 1;
        for (int y = -search_radius; y <= search_radius; y++) {
            for (int x = -search_radius; x <= search_radius; x++) {
                float2 uv                   = reprojection_uv + float2(x, y) * dudv;
                float3 history_normal       = FFX_DNSR_Reflections_SampleWorldSpaceNormalHistory(uv);
                float   history_depth        = FFX_DNSR_Reflections_SampleDepthHistory(uv);
                float   history_linear_depth = FFX_DNSR_Reflections_GetLinearDepth(uv, history_depth);
                float   weight               = FFX_DNSR_Reflections_GetDisocclusionFactor(normal, history_normal, linear_depth, history_linear_depth);
                if (weight > disocclusion_factor) {
                    disocclusion_factor = weight;
                    closest_uv          = uv;
                    reprojection_uv     = closest_uv;
                }
            }
        }
        reprojection = FFX_DNSR_Reflections_SampleRadianceHistory(reprojection_uv);
    }

    // Rare slow path - triggered only on the edges.
    // Try to get rid of potential leaks at bilinear interpolation level.
    if (disocclusion_factor < FFX_DNSR_REFLECTIONS_DISOCCLUSION_THRESHOLD) {
        // If we've got a discarded history, try to construct a better sample out of 2x2 interpolation neighborhood
        // Helps quite a bit on the edges in movement
        float   uvx                    = frac(float(screen_size.x) * reprojection_uv.x + 0.5);
        float   uvy                    = frac(float(screen_size.y) * reprojection_uv.y + 0.5);
        int2   reproject_texel_coords = int2(float2(screen_size) * reprojection_uv - 0.5);
        float3 reprojection00         = FFX_DNSR_Reflections_LoadRadianceHistory(reproject_texel_coords + int2(0, 0));
        float3 reprojection10         = FFX_DNSR_Reflections_LoadRadianceHistory(reproject_texel_coords + int2(1, 0));
        float3 reprojection01         = FFX_DNSR_Reflections_LoadRadianceHistory(reproject_texel_coords + int2(0, 1));
        float3 reprojection11         = FFX_DNSR_Reflections_LoadRadianceHistory(reproject_texel_coords + int2(1, 1));
        float3 normal00               = FFX_DNSR_Reflections_LoadWorldSpaceNormalHistory(reproject_texel_coords + int2(0, 0));
        float3 normal10               = FFX_DNSR_Reflections_LoadWorldSpaceNormalHistory(reproject_texel_coords + int2(1, 0));
        float3 normal01               = FFX_DNSR_Reflections_LoadWorldSpaceNormalHistory(reproject_texel_coords + int2(0, 1));
        float3 normal11               = FFX_DNSR_Reflections_LoadWorldSpaceNormalHistory(reproject_texel_coords + int2(1, 1));
        float   depth00                = FFX_DNSR_Reflections_GetLinearDepth(reprojection_uv, FFX_DNSR_Reflections_LoadDepthHistory(reproject_texel_coords + int2(0, 0)));
        float   depth10                = FFX_DNSR_Reflections_GetLinearDepth(reprojection_uv, FFX_DNSR_Reflections_LoadDepthHistory(reproject_texel_coords + int2(1, 0)));
        float   depth01                = FFX_DNSR_Reflections_GetLinearDepth(reprojection_uv, FFX_DNSR_Reflections_LoadDepthHistory(reproject_texel_coords + int2(0, 1)));
        float   depth11                = FFX_DNSR_Reflections_GetLinearDepth(reprojection_uv, FFX_DNSR_Reflections_LoadDepthHistory(reproject_texel_coords + int2(1, 1)));
        float4 w                      = float4(1.0f, 1.0f, 1.0f, 1.0f);
        // Initialize with occlusion weights
        w.x = FFX_DNSR_Reflections_GetDisocclusionFactor(normal, normal00, linear_depth, depth00) > FFX_DNSR_REFLECTIONS_DISOCCLUSION_THRESHOLD / 2.0f ? float(1.0f) : float(0.0f);
        w.y = FFX_DNSR_Reflections_GetDisocclusionFactor(normal, normal10, linear_depth, depth10) > FFX_DNSR_REFLECTIONS_DISOCCLUSION_THRESHOLD / 2.0f ? float(1.0f) : float(0.0f);
        w.z = FFX_DNSR_Reflections_GetDisocclusionFactor(normal, normal01, linear_depth, depth01) > FFX_DNSR_REFLECTIONS_DISOCCLUSION_THRESHOLD / 2.0f ? float(1.0f) : float(0.0f);
        w.w = FFX_DNSR_Reflections_GetDisocclusionFactor(normal, normal11, linear_depth, depth11) > FFX_DNSR_REFLECTIONS_DISOCCLUSION_THRESHOLD / 2.0f ? float(1.0f) : float(0.0f);
        // And then mix in bilinear weights
        w.x           = float(w.x * (1.0 - uvx) * (1.0 - uvy));
        w.y           = float(w.y * (uvx) * (1.0 - uvy));
        w.z           = float(w.z * (1.0 - uvx) * (uvy));
        w.w           = float(w.w * (uvx) * (uvy));
        float ws = max(w.x + w.y + w.z + w.w, float(1.0e-3));
        // normalize
        w /= ws;

        float3 history_normal;
        float       history_linear_depth;
        reprojection         = reprojection00 * w.x + reprojection10 * w.y + reprojection01 * w.z + reprojection11 * w.w;
        history_linear_depth = depth00 * w.x + depth10 * w.y + depth01 * w.z + depth11 * w.w;
        history_normal       = normal00 * w.x + normal10 * w.y + normal01 * w.z + normal11 * w.w;
        disocclusion_factor  = FFX_DNSR_Reflections_GetDisocclusionFactor(normal, history_normal, linear_depth, history_linear_depth);
    }
    disocclusion_factor = disocclusion_factor < FFX_DNSR_REFLECTIONS_DISOCCLUSION_THRESHOLD ? float(0.0f) : disocclusion_factor;
}

void FFX_DNSR_Reflections_Reproject(int2 dispatch_thread_id, int2 group_thread_id, uint2 screen_size, float temporal_stability_factor, int max_samples) {
    FFX_DNSR_Reflections_InitializeGroupSharedMemory(dispatch_thread_id, group_thread_id, int2(screen_size));
    GroupMemoryBarrierWithGroupSync();

    group_thread_id += 4; // Center threads in FFX_GROUPSHARED memory

    float       variance    = float(1.0f);
    float       num_samples = float(0.0f);
    float       roughness   = FFX_DNSR_Reflections_LoadRoughness(dispatch_thread_id);
    float3     normal      = FFX_DNSR_Reflections_LoadWorldSpaceNormal(dispatch_thread_id);
    float3     radiance    = FFX_DNSR_Reflections_LoadRadiance(dispatch_thread_id);
    const float ray_length  = FFX_DNSR_Reflections_LoadRayLength(dispatch_thread_id);

    if (FFX_DNSR_Reflections_IsGlossyReflection(roughness)) {
        float  disocclusion_factor;
        float2      reprojection_uv;
        float3 reprojection;
        FFX_DNSR_Reflections_PickReprojection(/*in*/ dispatch_thread_id,
                                              /* in */ group_thread_id,
                                              /* in */ screen_size,
                                              /* in */ roughness,
                                              /* in */ ray_length,
                                              /* out */ disocclusion_factor,
                                              /* out */ reprojection_uv,
                                              /* out */ reprojection);
        if ( (reprojection_uv.x > 0.0f) && (reprojection_uv.y > 0.0f) && (reprojection_uv.x < 1.0f) && (reprojection_uv.y< 1.0f)) {
            float prev_variance = FFX_DNSR_Reflections_SampleVarianceHistory(reprojection_uv);
            num_samples              = FFX_DNSR_Reflections_SampleNumSamplesHistory(reprojection_uv) * disocclusion_factor;
            float s_max_samples = float(max(8.0f, max_samples * FFX_DNSR_REFLECTIONS_SAMPLES_FOR_ROUGHNESS(roughness)));
            num_samples              = min(s_max_samples, num_samples + float(1.0f));
            float new_variance  = FFX_DNSR_Reflections_ComputeTemporalVariance(radiance.xyz, reprojection.xyz);
            if (disocclusion_factor < FFX_DNSR_REFLECTIONS_DISOCCLUSION_THRESHOLD) {
                FFX_DNSR_Reflections_StoreRadianceReprojected(dispatch_thread_id, float3(0.0f, 0.0f, 0.0f));
                FFX_DNSR_Reflections_StoreVariance(dispatch_thread_id, float(1.0f));
                FFX_DNSR_Reflections_StoreNumSamples(dispatch_thread_id, float(1.0f));
            } else {
                float variance_mix = lerp(new_variance, prev_variance, float(1.0f / num_samples));
                FFX_DNSR_Reflections_StoreRadianceReprojected(dispatch_thread_id, reprojection);
                FFX_DNSR_Reflections_StoreVariance(dispatch_thread_id, variance_mix);
                FFX_DNSR_Reflections_StoreNumSamples(dispatch_thread_id, num_samples);
                // Mix in reprojection for radiance mip computation
                radiance = lerp(radiance, reprojection, float(0.3f));
            }
        } else {
            FFX_DNSR_Reflections_StoreRadianceReprojected(dispatch_thread_id, float3(0.0f, 0.0f, 0.0f));
            FFX_DNSR_Reflections_StoreVariance(dispatch_thread_id, float(1.0f));
            FFX_DNSR_Reflections_StoreNumSamples(dispatch_thread_id, float(1.0f));
        }
    }

    // Downsample 8x8 -> 1 radiance using FFX_GROUPSHARED memory
    // Initialize FFX_GROUPSHARED array for downsampling
    float weight = FFX_DNSR_Reflections_GetLuminanceWeight(radiance.xyz);
    radiance.xyz *= weight;
    if ( (uint(dispatch_thread_id.x) >= screen_size.x) || (uint(dispatch_thread_id.y) >= screen_size.y) || !DenoiserIsFinite(radiance) || (weight > float(1.0e3))) {
        radiance = float3(0.0f, 0.0f, 0.0f);
        weight   = float(0.0f);
    }

    group_thread_id -= 4; // Center threads in FFX_GROUPSHARED memory

    FFX_DNSR_Reflections_StoreInGroupSharedMemory(group_thread_id, float4(radiance.xyz, weight));
    GroupMemoryBarrierWithGroupSync();

    for (int i = 2; i <= 8; i = i * 2) {
        int ox = group_thread_id.x * i;
        int oy = group_thread_id.y * i;
        int ix = group_thread_id.x * i + i / 2;
        int iy = group_thread_id.y * i + i / 2;
        if (ix < 8 && iy < 8) {
            float4 rad_weight00 = FFX_DNSR_Reflections_LoadFromGroupSharedMemoryRaw(int2(ox, oy));
            float4 rad_weight10 = FFX_DNSR_Reflections_LoadFromGroupSharedMemoryRaw(int2(ox, iy));
            float4 rad_weight01 = FFX_DNSR_Reflections_LoadFromGroupSharedMemoryRaw(int2(ix, oy));
            float4 rad_weight11 = FFX_DNSR_Reflections_LoadFromGroupSharedMemoryRaw(int2(ix, iy));
            float4 sum          = rad_weight00 + rad_weight01 + rad_weight10 + rad_weight11;
            FFX_DNSR_Reflections_StoreInGroupSharedMemory(int2(ox, oy), sum);
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if ((group_thread_id.x == 0) && (group_thread_id.y == 0)) {
        float4    sum          = FFX_DNSR_Reflections_LoadFromGroupSharedMemoryRaw(int2(0, 0));
        float      weight_acc   = max(sum.w, float(1.0e-3));
        float3    radiance_avg = sum.xyz / weight_acc;
        FFX_DNSR_Reflections_StoreAverageRadiance(dispatch_thread_id.xy / 8, float3(radiance_avg));
    }
}

void Reproject(uint group_index, uint group_id, uint2 group_thread_id) {
    uint   packed_coords = GetDenoiserTile(group_id);
    int2  dispatch_thread_id = int2(packed_coords & 0xffffu, (packed_coords >> 16) & 0xffffu) + int2(group_thread_id);
    int2  dispatch_group_id = dispatch_thread_id / 8;
    int2  remapped_group_thread_id = int2(DenoiserRemapForWaveReduction(group_index));
    int2  remapped_dispatch_thread_id = dispatch_group_id * 8 + remapped_group_thread_id;

    FFX_DNSR_Reflections_Reproject(remapped_dispatch_thread_id, remapped_group_thread_id, DenoiserRenderSize(), TemporalStabilityFactor(), 32);
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint LocalThreadIndex : SV_GroupIndex, uint3 WorkGroupId : SV_GroupID, uint3 GroupThreadId : SV_GroupThreadID)
{
    Reproject(LocalThreadIndex, WorkGroupId.x, GroupThreadId.xy);
}
