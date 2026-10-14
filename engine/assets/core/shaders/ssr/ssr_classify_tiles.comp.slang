// Screen-space reflections: decides which pixels trace a reflection ray and which 8×8 tiles the
// denoiser visits. Ports ffx_sssr_classify_tiles.h and its pass entry point
// (ffx_sssr_classify_tiles_pass.hlsl) from the FidelityFX SDK v1.1.4.
//
// One 64-thread workgroup covers an 8×8 tile, its threads remapped so each 2×2 quad occupies four
// consecutive local indices (RemapForWaveReduction). Per pixel it decides whether the pixel traces
// (glossy, reflective, picked as its quad's base ray or a mirror, or variance-guided), whether it
// copies a quad neighbour's ray instead, and appends the tracing pixels to the ray list with one
// counter add per subgroup. A rough reflective pixel takes the view's prefiltered environment along
// its mirror direction instead of a ray. A tile holding any glossy reflective pixel is appended to
// the denoiser tile list. The roughness it classified by is stored for the passes after it.
//
// Forced deviations from the reference:
// - The quad neighbours' copy requests are exchanged through workgroup memory rather than read
//   from lane index ^ 1/2/3: Vulkan does not promise that a subgroup's lanes are the workgroup's
//   local indices in order. The subgroup compaction keeps its wave operations, which are correct
//   under any lane order.
// - Roughness is the g-buffer's perceptual roughness squared (alpha), converted once here; every
//   later pass reads alpha, as the reference's "roughness" is.
// - The environment is the engine's prefiltered specular cube for the view, scaled by the view's
//   image-based-lighting intensity.
//
// A load past a texture's extent returns 0 and a store past it is dropped, the semantics the
// reference's loads and stores have on the threads of the last row and column of tiles.
//
// csCoverage is the engine's debug entry point, not the reference's: it runs the same
// classification and also writes what it decided for each pixel (ssr_coverage.slang) to a debug
// target.
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

#include "ssr_common.slang"
#include "ssr_coverage.slang"

[[vk::binding(0, 0)]] Texture2D g_Textures[];

[[vk::binding(1, 3)]] TextureCube g_EnvironmentMap;
[[vk::binding(2, 3)]] SamplerState g_EnvironmentMapSampler;
[[vk::binding(3, 3)]] Texture2D<float> g_DepthHierarchy;
[[vk::binding(4, 3)]] Texture2D<float> g_VarianceHistory;
[[vk::binding(5, 3)]] [format("rgba16f")] RWTexture2D<float4> g_Radiance;
[[vk::binding(6, 3)]] RWStructuredBuffer<uint> g_RayList;
[[vk::binding(7, 3)]] RWStructuredBuffer<uint> g_DenoiserTileList;
[[vk::binding(8, 3)]] globallycoherent RWStructuredBuffer<uint> g_RayCounter;
[[vk::binding(9, 3)]] [format("r8")] RWTexture2D<float> g_ExtractedRoughness;
[[vk::binding(10, 3)]] [format("r8")] RWTexture2D<float> g_Coverage; // csCoverage only

struct PushConstants
{
    uint NormalTexture;   // the g-buffer world normal's bindless index (G1)
    uint MaterialTexture; // the g-buffer occlusion/roughness/metallic's bindless index (G2)
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

bool IsInside(uint2 coordinate, uint2 extent)
{
    return all(coordinate < extent);
}

uint2 TextureExtent(Texture2D<float> texture)
{
    uint width;
    uint height;
    texture.GetDimensions(width, height);
    return uint2(width, height);
}

uint2 BindlessExtent(uint index)
{
    uint width;
    uint height;
    g_Textures[NonUniformResourceIndex(index)].GetDimensions(width, height);
    return uint2(width, height);
}

float3 FFX_SSSR_LoadWorldSpaceNormal(int2 pixel_coordinate)
{
    if (!IsInside(uint2(pixel_coordinate), BindlessExtent(g_PC.NormalTexture)))
    {
        return float3(0.0);
    }
    return normalize(g_Textures[NonUniformResourceIndex(g_PC.NormalTexture)].Load(int3(pixel_coordinate, 0)).xyz);
}

float FFX_SSSR_LoadDepth(int2 pixel_coordinate, int mip)
{
    if (!IsInside(uint2(pixel_coordinate), max(TextureExtent(g_DepthHierarchy) >> uint(mip), uint2(1))))
    {
        return 0.0;
    }
    return g_DepthHierarchy.Load(int3(pixel_coordinate, mip));
}

float FFX_SSSR_LoadVarianceHistory(int3 coordinate)
{
    if (!IsInside(uint2(coordinate.xy), TextureExtent(g_VarianceHistory)))
    {
        return 0.0;
    }
    return g_VarianceHistory.Load(coordinate).x;
}

// The reference squares a perceptual input once at this load when told it is perceptual; the
// engine's g-buffer always is.
float LoadRoughnessFromMaterialParametersInput(uint3 coordinate)
{
    if (!IsInside(coordinate.xy, BindlessExtent(g_PC.MaterialTexture)))
    {
        return 0.0;
    }
    float rawRoughness = g_Textures[NonUniformResourceIndex(g_PC.MaterialTexture)].Load(int3(coordinate)).g;
    rawRoughness *= rawRoughness;
    return rawRoughness;
}

bool IsReflectiveSurface(int2 pixel_coordinate, float roughness)
{
    const float far_plane = 0.0f;
    return FFX_SSSR_LoadDepth(pixel_coordinate, 0) > far_plane;
}

float3 FFX_SSSR_SampleEnvironmentMap(float3 direction, float preceptualRoughness)
{
    uint width;
    uint height;
    g_EnvironmentMap.GetDimensions(width, height);
    float Width = float(width);
    int maxMipLevel = int(log2(float(Width > 0 ? Width : 1)));
    float mip = clamp(preceptualRoughness * float(maxMipLevel), 0.0, float(maxMipLevel));
    return g_EnvironmentMap.SampleLevel(g_EnvironmentMapSampler, direction, mip).xyz * IBLFactor() * IBLTint();
}

void FFX_SSSR_StoreRadiance(uint2 coordinate, float4 radiance)
{
    uint width;
    uint height;
    g_Radiance.GetDimensions(width, height);
    if (IsInside(coordinate, uint2(width, height)))
    {
        g_Radiance[coordinate] = radiance;
    }
}

void StoreExtractedRoughness(uint2 coordinate, float roughness)
{
    uint width;
    uint height;
    g_ExtractedRoughness.GetDimensions(width, height);
    if (IsInside(coordinate, uint2(width, height)))
    {
        g_ExtractedRoughness[coordinate] = roughness;
    }
}

void IncrementRayCounter(uint value, out uint original_value)
{
    InterlockedAdd(g_RayCounter[0], value, original_value);
}

void IncrementDenoiserTileCounter(out uint original_value)
{
    InterlockedAdd(g_RayCounter[2], 1, original_value);
}

void StoreRay(int index, uint2 ray_coord, bool copy_horizontal, bool copy_vertical, bool copy_diagonal)
{
    uint packedRayCoords = PackRayCoords(ray_coord, copy_horizontal, copy_vertical, copy_diagonal); // Store out pixel to trace
    g_RayList[index] = packedRayCoords;
}

void StoreDenoiserTile(int index, uint2 tile_coord)
{
    g_DenoiserTileList[index] = ((tile_coord.y & 0xffffu) << 16) | ((tile_coord.x & 0xffffu) << 0); // Store out pixel to trace
}

// ffxRemapForWaveReduction: local index -> 8×8 position, each 2×2 quad on four consecutive indices.
uint BitfieldExtract(uint src, uint off, uint bits)
{
    uint mask = (1u << bits) - 1;
    return (src >> off) & mask;
}

uint BitfieldInsertMask(uint src, uint ins, uint bits)
{
    uint mask = (1u << bits) - 1;
    return (ins & mask) | (src & (~mask));
}

uint2 RemapForWaveReduction(uint a)
{
    return uint2(BitfieldInsertMask(BitfieldExtract(a, 2u, 3u), a, 1u), BitfieldInsertMask(BitfieldExtract(a, 3u, 3u), BitfieldExtract(a, 1u, 2u), 2u));
}

bool IsBaseRay(uint2 dispatch_thread_id, uint samples_per_quad)
{
    switch (samples_per_quad)
    {
    case 1:
        return ((dispatch_thread_id.x & 1) | (dispatch_thread_id.y & 1)) == 0; // Deactivates 3 out of 4 rays
    case 2:
        return (dispatch_thread_id.x & 1) == (dispatch_thread_id.y & 1); // Deactivates 2 out of 4 rays. Keeps diagonal.
    default: // case 4:
        return true;
    }
}

groupshared uint g_TileCount;
// Each thread's require_copy, indexed by its local index, so a quad partner reads it at
// local index ^ 1 (horizontal), ^ 2 (vertical) and ^ 3 (diagonal).
groupshared uint g_RequireCopy[64];

// What ClassifyTiles decided for its pixel, which csCoverage reports.
struct ClassifyDecisions
{
    bool needs_ray;
    bool needs_denoiser;
    bool is_base_ray;
    bool require_copy;
    bool copy_horizontal;
    bool copy_vertical;
    bool copy_diagonal;
    bool is_reflective_surface;
    bool is_glossy_reflection;
};

void ClassifyTiles(uint2 dispatch_thread_id, uint2 group_thread_id, uint local_thread_index, float roughness, out ClassifyDecisions decisions)
{
    g_TileCount = 0;

    bool is_first_lane_of_wave = WaveIsFirstLane();

    // First we figure out on a per thread basis if we need to shoot a reflection ray.
    // Disable offscreen pixels
    bool needs_ray = !(dispatch_thread_id.x >= RenderSize().x || dispatch_thread_id.y >= RenderSize().y);

    // Dont shoot a ray on very rough surfaces.
    bool is_reflective_surface = IsReflectiveSurface(int2(dispatch_thread_id), roughness);
    bool is_glossy_reflection = IsGlossyReflection(roughness);
    needs_ray = needs_ray && is_glossy_reflection && is_reflective_surface;

    // Also we dont need to run the denoiser on mirror reflections.
    bool needs_denoiser = needs_ray && !IsMirrorReflection(roughness);

    // Decide which ray to keep
    bool is_base_ray = IsBaseRay(dispatch_thread_id, SamplesPerQuad());
    needs_ray = needs_ray && (!needs_denoiser || is_base_ray); // Make sure to not deactivate mirror reflection rays.

    if (TemporalVarianceGuidedTracingEnabled() && needs_denoiser && !needs_ray)
    {
        bool has_temporal_variance = FFX_SSSR_LoadVarianceHistory(int3(int2(dispatch_thread_id), 0)) > VarianceThreshold();
        needs_ray = needs_ray || has_temporal_variance;
    }

    // Our pixel only requires a copy if we want to run a denoiser on it but don't want to shoot a ray for it.
    bool require_copy = !needs_ray && needs_denoiser;
    g_RequireCopy[local_thread_index] = require_copy ? 1u : 0u;

    GroupMemoryBarrierWithGroupSync(); // Wait until g_TileCount is cleared - allow some computations before and after

    // Now we know for each thread if it needs to shoot a ray and wether or not a denoiser pass has to run on this pixel.

    if (is_glossy_reflection && is_reflective_surface)
    {
        InterlockedAdd(g_TileCount, 1);
    }

    // Next we have to figure out for which pixels that ray is creating the values for. Thus, if we have to copy its value horizontal, vertical or across.
    bool copy_horizontal = g_RequireCopy[local_thread_index ^ 1] != 0u && (SamplesPerQuad() != 4) && is_base_ray; // QuadReadAcrossX
    bool copy_vertical = g_RequireCopy[local_thread_index ^ 2] != 0u && (SamplesPerQuad() == 1) && is_base_ray;   // QuadReadAcrossY
    bool copy_diagonal = g_RequireCopy[local_thread_index ^ 3] != 0u && (SamplesPerQuad() == 1) && is_base_ray;   // QuadReadAcrossDiagonal

    // Thus, we need to compact the rays and append them all at once to the ray list.
    uint local_ray_index_in_wave = WavePrefixCountBits(needs_ray);
    uint wave_ray_count = WaveActiveCountBits(needs_ray);
    uint base_ray_index = 0;
    if (is_first_lane_of_wave)
    {
        IncrementRayCounter(wave_ray_count, base_ray_index);
    }
    base_ray_index = WaveReadLaneFirst(base_ray_index);
    if (needs_ray)
    {
        int ray_index = int(base_ray_index + local_ray_index_in_wave);
        StoreRay(ray_index, dispatch_thread_id, copy_horizontal, copy_vertical, copy_diagonal);
    }

    float4 intersection_output = float4(0.0f, 0.0f, 0.0f, 0.0f);
    if (is_reflective_surface && !is_glossy_reflection)
    {
        // Fall back to environment map without preparing a ray
        float2 uv = (float2(dispatch_thread_id) + 0.5) * InverseRenderSize();
        float3 world_space_normal = FFX_SSSR_LoadWorldSpaceNormal(int2(dispatch_thread_id));
        float z = FFX_SSSR_LoadDepth(int2(dispatch_thread_id), 0);
        float3 screen_uv_space_ray_origin = float3(uv, z);
        float3 view_space_ray = ScreenSpaceToViewSpace(screen_uv_space_ray_origin);
        float3 view_space_ray_direction = normalize(view_space_ray);
        float3 view_space_surface_normal = mul(ViewMatrix(), float4(world_space_normal, 0)).xyz;
        float3 view_space_reflected_direction = reflect(view_space_ray_direction, view_space_surface_normal);
        float3 world_space_reflected_direction = mul(InvView(), float4(view_space_reflected_direction, 0)).xyz;

        intersection_output.xyz = FFX_SSSR_SampleEnvironmentMap(world_space_reflected_direction, sqrt(roughness));
    }

    FFX_SSSR_StoreRadiance(dispatch_thread_id, intersection_output);

    decisions.needs_ray = needs_ray;
    decisions.needs_denoiser = needs_denoiser;
    decisions.is_base_ray = is_base_ray;
    decisions.require_copy = require_copy;
    decisions.copy_horizontal = copy_horizontal;
    decisions.copy_vertical = copy_vertical;
    decisions.copy_diagonal = copy_diagonal;
    decisions.is_reflective_surface = is_reflective_surface;
    decisions.is_glossy_reflection = is_glossy_reflection;

    GroupMemoryBarrierWithGroupSync(); // Wait until g_TileCount

    if ((group_thread_id.x == 0) && (group_thread_id.y == 0) && g_TileCount > 0)
    {
        uint tile_offset;
        IncrementDenoiserTileCounter(tile_offset);
        StoreDenoiserTile(int(tile_offset), dispatch_thread_id.xy);
    }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void csMain(uint LocalThreadIndex : SV_GroupIndex, uint3 WorkGroupId : SV_GroupID)
{
    uint2 group_thread_id = RemapForWaveReduction(LocalThreadIndex);
    uint2 dispatch_thread_id = WorkGroupId.xy * 8 + group_thread_id;
    float roughness = LoadRoughnessFromMaterialParametersInput(uint3(dispatch_thread_id, 0));

    ClassifyDecisions decisions;
    ClassifyTiles(dispatch_thread_id, group_thread_id, LocalThreadIndex, roughness, decisions);

    // Extract only the channel containing the roughness to avoid loading all 4 channels in the follow up passes.
    StoreExtractedRoughness(dispatch_thread_id, roughness);
}

groupshared uint g_CoverageCode[64];

[shader("compute")]
[numthreads(64, 1, 1)]
void csCoverage(uint LocalThreadIndex : SV_GroupIndex, uint3 WorkGroupId : SV_GroupID)
{
    uint2 group_thread_id = RemapForWaveReduction(LocalThreadIndex);
    uint2 dispatch_thread_id = WorkGroupId.xy * 8 + group_thread_id;
    float roughness = LoadRoughnessFromMaterialParametersInput(uint3(dispatch_thread_id, 0));

    ClassifyDecisions decisions;
    ClassifyTiles(dispatch_thread_id, group_thread_id, LocalThreadIndex, roughness, decisions);
    StoreExtractedRoughness(dispatch_thread_id, roughness);

    // A pixel's own decision first; a rough pixel's environment is scaled by the IBL factor, so with
    // none it receives nothing.
    uint code = SsrCoverageNone;
    if (decisions.needs_ray)
    {
        code = !decisions.needs_denoiser ? SsrCoverageMirror
                                         : (decisions.is_base_ray ? SsrCoverageRay : SsrCoverageVarianceRay);
    }
    else if (decisions.require_copy)
    {
        code = SsrCoverageUnreached;
    }
    else if (decisions.is_reflective_surface && !decisions.is_glossy_reflection && IBLFactor() > 0.0)
    {
        code = SsrCoverageEnvironment;
    }
    g_CoverageCode[LocalThreadIndex] = code;
    GroupMemoryBarrierWithGroupSync();

    // A traced ray's copy flags then mark the quad neighbours the intersection copies it to, at the
    // same local indices the classifier's copy exchange reads.
    if (decisions.needs_ray)
    {
        if (decisions.copy_horizontal)
        {
            g_CoverageCode[LocalThreadIndex ^ 1] = SsrCoverageCopied;
        }
        if (decisions.copy_vertical)
        {
            g_CoverageCode[LocalThreadIndex ^ 2] = SsrCoverageCopied;
        }
        if (decisions.copy_diagonal)
        {
            g_CoverageCode[LocalThreadIndex ^ 3] = SsrCoverageCopied;
        }
    }
    GroupMemoryBarrierWithGroupSync();

    if (all(dispatch_thread_id < RenderSize()))
    {
        g_Coverage[dispatch_thread_id] = float(g_CoverageCode[LocalThreadIndex]) / 255.0;
    }
}
