// Screen-space reflections: traces each listed ray through the depth hierarchy and stores the
// reflected radiance. Ports ffx_sssr_intersect.h (with the callbacks of ffx_sssr_callbacks_hlsl.h it
// binds) from the FidelityFX SDK v1.1.4.
//
// One 64-thread workgroup traces 64 consecutive rays of the list, one per thread; the dispatch is
// indirect, sized by the indirect-argument pass. A ray samples a GGX visible normal (Heitz 2018)
// from the blue-noise tile, marches the hierarchy from its most detailed level, and is weighed by
// ValidateHit. Its radiance is the environment along the sampled direction blended towards this
// frame's lit colour at the hit by the hit's confidence; the ray's world-space length rides alpha.
// A base ray also writes its result to each quad neighbour its copy flags name.
//
// The engine's depth is reverse-Z (the reference's FFX_SSSR_OPTION_INVERTED_DEPTH). The environment
// is the view's prefiltered specular cube scaled by the view's image-based-lighting intensity, as
// the classifier samples it. A load past a texture's extent or level count returns 0 and a store
// past it is dropped, the semantics the reference's loads and stores have.
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

[[vk::binding(0, 0)]] Texture2D g_Textures[];

[[vk::binding(1, 3)]] TextureCube g_EnvironmentMap;
[[vk::binding(2, 3)]] SamplerState g_EnvironmentMapSampler;
[[vk::binding(3, 3)]] Texture2D<float> g_DepthHierarchy;
[[vk::binding(4, 3)]] Texture2D<float> g_ExtractedRoughness;
[[vk::binding(5, 3)]] Texture2D<float2> g_BlueNoiseTexture;
[[vk::binding(6, 3)]] RWStructuredBuffer<uint> g_RayList;
[[vk::binding(7, 3)]] globallycoherent RWStructuredBuffer<uint> g_RayCounter;
[[vk::binding(8, 3)]] [format("rgba16f")] RWTexture2D<float4> g_Radiance;

struct PushConstants
{
    uint NormalTexture; // the g-buffer world normal's bindless index (G1)
    uint ColorTexture;  // this frame's lit opaque colour's bindless index
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

#define M_PI                               3.14159265358979f
#define FFX_SSSR_FLOAT_MAX                 3.402823466e+38
#define FFX_SSSR_DEPTH_HIERARCHY_MAX_MIP   6

//=== The callbacks this pass binds ===

bool IsInside(uint2 coordinate, uint2 extent)
{
    return all(coordinate < extent);
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
    uint width;
    uint height;
    uint levels;
    g_DepthHierarchy.GetDimensions(0, width, height, levels);
    if (uint(mip) >= levels || !IsInside(uint2(pixel_coordinate), max(uint2(width, height) >> uint(mip), uint2(1))))
    {
        return 0.0;
    }
    return g_DepthHierarchy.Load(int3(pixel_coordinate, mip));
}

float FFX_SSSR_LoadExtractedRoughness(int3 coordinate)
{
    uint width;
    uint height;
    g_ExtractedRoughness.GetDimensions(width, height);
    if (!IsInside(uint2(coordinate.xy), uint2(width, height)))
    {
        return 0.0;
    }
    return g_ExtractedRoughness.Load(coordinate).x;
}

float2 FFX_SSSR_SampleRandomVector2D(uint2 pixel)
{
    return g_BlueNoiseTexture.Load(int3(int2(pixel.xy % 128), 0));
}

float3 FFX_SSSR_SampleEnvironmentMap(float3 direction, float preceptualRoughness)
{
    uint width;
    uint height;
    g_EnvironmentMap.GetDimensions(width, height);
    float Width = float(width);
    int maxMipLevel = int(log2(float(Width > 0 ? Width : 1)));
    float mip = clamp(preceptualRoughness * float(maxMipLevel), 0.0, float(maxMipLevel));
    return g_EnvironmentMap.SampleLevel(g_EnvironmentMapSampler, direction, mip).xyz * IBLFactor();
}

float3 FFX_SSSR_LoadInputColor(int3 coordinate)
{
    if (!IsInside(uint2(coordinate.xy), BindlessExtent(g_PC.ColorTexture)))
    {
        return float3(0.0);
    }
    return g_Textures[NonUniformResourceIndex(g_PC.ColorTexture)].Load(coordinate).xyz;
}

bool IsRayIndexValid(uint ray_index)
{
    return ray_index < g_RayCounter[1];
}

uint GetRaylist(uint ray_index)
{
    return g_RayList[ray_index];
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

//=== ffx_sssr_intersect.h ===

float3 FFX_SSSR_ScreenSpaceToViewSpace(float3 screen_space_position)
{
    return InvProjectPosition(screen_space_position, InvProjection());
}

float3 ScreenSpaceToWorldSpace(float3 screen_space_position)
{
    return InvProjectPosition(screen_space_position, InvViewProjection());
}

// http://jcgt.org/published/0007/04/01/paper.pdf by Eric Heitz
// Input Ve: view direction
// Input alpha_x, alpha_y: roughness parameters
// Input U1, U2: uniform random numbers
// Output Ne: normal sampled with PDF D_Ve(Ne) = G1(Ve) * max(0, dot(Ve, Ne)) * D(Ne) / Ve.z
float3 SampleGGXVNDF(float3 Ve, float alpha_x, float alpha_y, float U1, float U2)
{
    // Section 3.2: transforming the view direction to the hemisphere configuration
    float3 Vh = normalize(float3(alpha_x * Ve.x, alpha_y * Ve.y, Ve.z));
    // Section 4.1: orthonormal basis (with special case if cross product is zero)
    float lensq = Vh.x * Vh.x + Vh.y * Vh.y;
    float3 T1 = lensq > 0 ? float3(-Vh.y, Vh.x, 0) * rsqrt(lensq) : float3(1, 0, 0);
    float3 T2 = cross(Vh, T1);
    // Section 4.2: parameterization of the projected area
    float r = sqrt(U1);
    float phi = 2.0 * M_PI * U2;
    float t1 = r * cos(phi);
    float t2 = r * sin(phi);
    float s = 0.5 * (1.0 + Vh.z);
    t2 = (1.0 - s) * sqrt(1.0 - t1 * t1) + s * t2;
    // Section 4.3: reprojection onto hemisphere
    float3 Nh = t1 * T1 + t2 * T2 + sqrt(max(0.0, 1.0 - t1 * t1 - t2 * t2)) * Vh;
    // Section 3.4: transforming the normal back to the ellipsoid configuration
    float3 Ne = normalize(float3(alpha_x * Nh.x, alpha_y * Nh.y, max(0.0, Nh.z)));
    return Ne;
}

float3 Sample_GGX_VNDF_Ellipsoid(float3 Ve, float alpha_x, float alpha_y, float U1, float U2)
{
    return SampleGGXVNDF(Ve, alpha_x, alpha_y, U1, U2);
}

float3 Sample_GGX_VNDF_Hemisphere(float3 Ve, float alpha, float U1, float U2)
{
    return Sample_GGX_VNDF_Ellipsoid(Ve, alpha, alpha, U1, U2);
}

float3 SampleReflectionVector(float3 view_direction, float3 normal, float roughness, int2 dispatch_thread_id)
{
    float3 U;
    float3 N = normal;
    if (abs(N.z) > 0.0)
    {
        float k = sqrt(N.y * N.y + N.z * N.z);
        U.x = 0.0; U.y = -N.z / k; U.z = N.y / k;
    }
    else
    {
        float k = sqrt(N.x * N.x + N.y * N.y);
        U.x = N.y / k; U.y = -N.x / k; U.z = 0.0;
    }

    // TBN 3x3 matrix
    float3 TBN_row0 = U;
    float3 TBN_row1 = cross(N, U);
    float3 TBN_row2 = N;

    // TBN * -view_direction
    float3 view_direction_tbn = float3(dot(TBN_row0, -view_direction), dot(TBN_row1, -view_direction), dot(TBN_row2, -view_direction));

    float2 u = FFX_SSSR_SampleRandomVector2D(uint2(dispatch_thread_id));

    float3 sampled_normal_tbn = Sample_GGX_VNDF_Hemisphere(view_direction_tbn, roughness, u.x, u.y);

    float3 reflected_direction_tbn = reflect(-view_direction_tbn, sampled_normal_tbn);

    // Transpose of TBN
    float3 TBN_col0 = float3(TBN_row0[0], TBN_row1[0], TBN_row2[0]);
    float3 TBN_col1 = float3(TBN_row0[1], TBN_row1[1], TBN_row2[1]);
    float3 TBN_col2 = float3(TBN_row0[2], TBN_row1[2], TBN_row2[2]);

    // transpose(TBN) * reflected_direction_tbn
    return float3(dot(TBN_col0, reflected_direction_tbn), dot(TBN_col1, reflected_direction_tbn), dot(TBN_col2, reflected_direction_tbn));
}

void FFX_SSSR_InitialAdvanceRay(float3 origin, float3 direction, float3 inv_direction, float2 current_mip_resolution, float2 current_mip_resolution_inv, float2 floor_offset, float2 uv_offset, out float3 position, out float current_t)
{
    float2 current_mip_position = current_mip_resolution * origin.xy;

    // Intersect ray with the half box that is pointing away from the ray origin.
    float2 xy_plane = floor(current_mip_position) + floor_offset;
    xy_plane = xy_plane * current_mip_resolution_inv + uv_offset;

    // o + d * t = p' => t = (p' - o) / d
    float2 t = xy_plane * inv_direction.xy - origin.xy * inv_direction.xy;
    current_t = min(t.x, t.y);
    position = origin + current_t * direction;
}

bool FFX_SSSR_AdvanceRay(float3 origin, float3 direction, float3 inv_direction, float2 current_mip_position, float2 current_mip_resolution_inv, float2 floor_offset, float2 uv_offset, float surface_z, inout float3 position, inout float current_t)
{
    // Create boundary planes
    float2 xy_plane = floor(current_mip_position) + floor_offset;
    xy_plane = xy_plane * current_mip_resolution_inv + uv_offset;
    float3 boundary_planes = float3(xy_plane, surface_z);

    // Intersect ray with the half box that is pointing away from the ray origin.
    // o + d * t = p' => t = (p' - o) / d
    float3 t = boundary_planes * inv_direction - origin * inv_direction;

    // Prevent using z plane when shooting out of the depth buffer.
    t.z = direction.z < 0 ? t.z : FFX_SSSR_FLOAT_MAX;

    // Choose nearest intersection with a boundary.
    float t_min = min(min(t.x, t.y), t.z);

    // Larger z means closer to the camera.
    bool above_surface = surface_z < position.z;

    // Decide whether we are able to advance the ray until we hit the xy boundaries or if we had to clamp it at the surface.
    // We use the asuint comparison to avoid NaN / Inf logic, also we actually care about bitwise equality here to see if t_min is the t.z we fed into the min3 above.
    bool skipped_tile = asuint(t_min) != asuint(t.z) && above_surface;

    // Make sure to only advance the ray if we're still above the surface.
    current_t = above_surface ? t_min : current_t;

    // Advance ray
    position = origin + current_t * direction;

    return skipped_tile;
}

float2 FFX_SSSR_GetMipResolution(float2 screen_dimensions, int mip_level)
{
    return screen_dimensions * pow(0.5, float(mip_level));
}

// Requires origin and direction of the ray to be in screen space [0, 1] x [0, 1]
float3 FFX_SSSR_HierarchicalRaymarch(float3 origin, float3 direction, bool is_mirror, float2 screen_size, int most_detailed_mip, uint min_traversal_occupancy, uint max_traversal_intersections, out bool valid_hit)
{
    const float3 inv_direction = float3(direction.x != 0.0f ? 1.0f / direction.x : FFX_SSSR_FLOAT_MAX,
                                        direction.y != 0.0f ? 1.0f / direction.y : FFX_SSSR_FLOAT_MAX,
                                        direction.z != 0.0f ? 1.0f / direction.z : FFX_SSSR_FLOAT_MAX);

    // Start on mip with highest detail.
    int current_mip = most_detailed_mip;

    // Could recompute these every iteration, but it's faster to hoist them out and update them.
    float2 current_mip_resolution = FFX_SSSR_GetMipResolution(screen_size, current_mip);
    float2 current_mip_resolution_inv = 1.0f / current_mip_resolution;

    // Offset to the bounding boxes uv space to intersect the ray with the center of the next pixel.
    // This means we ever so slightly over shoot into the next region.
    float2 uv_offset = 0.005 * exp2(float(most_detailed_mip)) / screen_size;
    uv_offset.x = direction.x < 0.0f ? -uv_offset.x : uv_offset.x;
    uv_offset.y = direction.y < 0.0f ? -uv_offset.y : uv_offset.y;

    // Offset applied depending on current mip resolution to move the boundary to the left/right upper/lower border depending on ray direction.
    float2 floor_offset;
    floor_offset.x = direction.x < 0.0f ? 0.0f : 1.0f;
    floor_offset.y = direction.y < 0.0f ? 0.0f : 1.0f;

    // Initially advance ray to avoid immediate self intersections.
    float current_t;
    float3 position;
    FFX_SSSR_InitialAdvanceRay(origin, direction, inv_direction, current_mip_resolution, current_mip_resolution_inv, floor_offset, uv_offset, position, current_t);

    bool exit_due_to_low_occupancy = false;
    int i = 0;
    while (uint(i) < max_traversal_intersections && current_mip >= most_detailed_mip && !exit_due_to_low_occupancy)
    {
        float2 current_mip_position = current_mip_resolution * position.xy;
        float surface_z = FFX_SSSR_LoadDepth(int2(current_mip_position), current_mip);
        exit_due_to_low_occupancy = !is_mirror && WaveActiveCountBits(true) <= min_traversal_occupancy;
        bool skipped_tile = FFX_SSSR_AdvanceRay(origin, direction, inv_direction, current_mip_position, current_mip_resolution_inv, floor_offset, uv_offset, surface_z, position, current_t);

        // Don't increase mip further than this because we did not generate it
        bool nextMipIsOutOfRange = skipped_tile && (current_mip >= FFX_SSSR_DEPTH_HIERARCHY_MAX_MIP);
        if (!nextMipIsOutOfRange)
        {
            current_mip += skipped_tile ? 1 : -1;
            current_mip_resolution *= skipped_tile ? 0.5 : 2.0;
            current_mip_resolution_inv *= skipped_tile ? 2.0 : 0.5;
        }

        ++i;
    }

    valid_hit = (uint(i) <= max_traversal_intersections);

    return position;
}

float FFX_SSSR_ValidateHit(float3 hit, float2 uv, float3 world_space_ray_direction, float2 screen_size, float depth_buffer_thickness)
{
    // Reject hits outside the view frustum
    if ((hit.x < 0.0f) || (hit.y < 0.0f) || (hit.x > 1.0f) || (hit.y > 1.0f))
    {
        return 0.0f;
    }

    // Reject the hit if we didnt advance the ray significantly to avoid immediate self reflection
    float2 manhattan_dist = abs(hit.xy - uv);
    if ((manhattan_dist.x < (2.0f / screen_size.x)) && (manhattan_dist.y < (2.0f / screen_size.y)))
    {
        return 0.0;
    }

    // Don't lookup radiance from the background.
    int2 texel_coords = int2(screen_size * hit.xy);
    float surface_z = FFX_SSSR_LoadDepth(texel_coords / 2, 1);
    if (surface_z == 0.0)
    {
        return 0.0;
    }

    // We check if we hit the surface from the back, these should be rejected.
    float3 hit_normal = FFX_SSSR_LoadWorldSpaceNormal(texel_coords);
    if (dot(hit_normal, world_space_ray_direction) > 0)
    {
        return 0.0;
    }

    float3 view_space_surface = FFX_SSSR_ScreenSpaceToViewSpace(float3(hit.xy, surface_z));
    float3 view_space_hit = FFX_SSSR_ScreenSpaceToViewSpace(hit);
    float distance = length(view_space_surface - view_space_hit);

    // Fade out hits near the screen borders
    float2 fov = 0.05 * float2(screen_size.y / screen_size.x, 1);
    float2 border = smoothstep(float2(0.0f, 0.0f), fov, hit.xy) * (1 - smoothstep(float2(1.0f, 1.0f) - fov, float2(1.0f, 1.0f), hit.xy));
    float vignette = border.x * border.y;

    // We accept all hits that are within a reasonable minimum distance below the surface.
    // Add constant in linear space to avoid growing of the reflections toward the reflected objects.
    float confidence = 1.0f - smoothstep(0.0f, depth_buffer_thickness, distance);
    confidence *= confidence;

    return vignette * confidence;
}

void Intersect(uint group_index, uint group_id)
{
    uint ray_index = group_id * 64 + group_index;
    if (!IsRayIndexValid(ray_index))
    {
        return;
    }

    uint packed_coords = GetRaylist(ray_index);

    uint2 coords;
    bool copy_horizontal;
    bool copy_vertical;
    bool copy_diagonal;
    UnpackRayCoords(packed_coords, coords, copy_horizontal, copy_vertical, copy_diagonal);

    const uint2 screen_size = RenderSize();

    float2 uv = (float2(coords) + 0.5) * InverseRenderSize();

    float3 world_space_normal = FFX_SSSR_LoadWorldSpaceNormal(int2(coords));
    float roughness = FFX_SSSR_LoadExtractedRoughness(int3(int2(coords), 0));
    bool is_mirror = IsMirrorReflection(roughness);

    int most_detailed_mip = is_mirror ? 0 : int(MostDetailedMip());
    float2 mip_resolution = FFX_SSSR_GetMipResolution(float2(screen_size), most_detailed_mip);
    float z = FFX_SSSR_LoadDepth(int2(uv * mip_resolution), most_detailed_mip);

    float3 screen_uv_space_ray_origin = float3(uv, z);
    float3 view_space_ray = ScreenSpaceToViewSpace(screen_uv_space_ray_origin);
    float3 view_space_ray_direction = normalize(view_space_ray);

    float3 view_space_surface_normal = mul(ViewMatrix(), float4(world_space_normal, 0)).xyz;
    float3 view_space_reflected_direction = SampleReflectionVector(view_space_ray_direction, view_space_surface_normal, roughness, int2(coords));
    float3 screen_space_ray_direction = ProjectDirection(view_space_ray, view_space_reflected_direction, screen_uv_space_ray_origin, Projection());

    //====SSSR====
    bool valid_hit = false;
    float3 hit = FFX_SSSR_HierarchicalRaymarch(screen_uv_space_ray_origin, screen_space_ray_direction, is_mirror, float2(screen_size), most_detailed_mip, MinTraversalOccupancy(), MaxTraversalIntersections(), valid_hit);

    float3 world_space_origin = ScreenSpaceToWorldSpace(screen_uv_space_ray_origin);
    float3 world_space_hit = ScreenSpaceToWorldSpace(hit);
    float3 world_space_ray = world_space_hit - world_space_origin.xyz;

    float confidence = valid_hit ? FFX_SSSR_ValidateHit(hit, uv, world_space_ray, float2(screen_size), DepthBufferThickness()) : 0.0;
    float world_ray_length = max(0.0, length(world_space_ray));

    float3 reflection_radiance = float3(0.0f, 0.0f, 0.0f);
    if (confidence > 0.0f)
    {
        // Found an intersection with the depth buffer -> We can lookup the color from lit scene.
        reflection_radiance = FFX_SSSR_LoadInputColor(int3(int2(float2(screen_size) * hit.xy), 0));
    }

    // Sample environment map.
    float3 world_space_reflected_direction = mul(InvView(), float4(view_space_reflected_direction, 0)).xyz;
    float3 environment_lookup = FFX_SSSR_SampleEnvironmentMap(world_space_reflected_direction, 0.0f);
    reflection_radiance = lerp(environment_lookup, reflection_radiance, confidence);

    float4 new_sample = float4(reflection_radiance, world_ray_length);

    FFX_SSSR_StoreRadiance(coords, new_sample);

    uint2 copy_target = coords ^ 1u; // Flip last bit to find the mirrored coords along the x and y axis within a quad.
    if (copy_horizontal)
    {
        uint2 copy_coords = uint2(copy_target.x, coords.y);
        FFX_SSSR_StoreRadiance(copy_coords, new_sample);
    }
    if (copy_vertical)
    {
        uint2 copy_coords = uint2(coords.x, copy_target.y);
        FFX_SSSR_StoreRadiance(copy_coords, new_sample);
    }
    if (copy_diagonal)
    {
        uint2 copy_coords = copy_target;
        FFX_SSSR_StoreRadiance(copy_coords, new_sample);
    }
}

[shader("compute")]
[numthreads(64, 1, 1)]
void csMain(uint group_index : SV_GroupIndex, uint3 group_id : SV_GroupID)
{
    Intersect(group_index, group_id.x);
}
