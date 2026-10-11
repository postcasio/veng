// Stochastic SSR intersection: traces each listed ray through the nearest-depth pyramid and writes
// the lit scene it meets, premultiplied by the hit's confidence, with the ray's length beside it.
// One invocation per ray, 64 to a group, dispatched indirectly by the ray count.
//
// - The direction is a GGX sample of the pixel's lobe (Veng/ssr_sample.slang) from the rotated
//   blue-noise tile; a mirror reflects exactly.
// - The march works in screen UV plus device depth, on a straight line from the pixel's own point.
//   Each iteration meets the current cell's x and y planes and its depth plane, the latter only
//   while the ray heads away from the camera. A ray that leaves its cell above the cell's nearest
//   depth climbs a level, up to MaxTraversalMip; one that meets a surface descends, and the march
//   ends when it descends below level 0. Every cell is 2^level level-0 texels wide on screen;
//   the pyramid's floor-halved levels each cover their cells (ssr_hiz_reduce.comp's footprints), so
//   the cell's depth is read clamped into the level.
// - A ray that spends its iteration budget, leaves the screen, or is stopped by low subgroup
//   occupancy has met nothing, and is a miss.
// - Depth is resolved to the end of the line. A ray that descends below level 0 behind the
//   surface it stands over crossed that surface's depth either inside the texel, past the cell
//   boundary it overshot onto, where the hit moves back to the crossing, or before the texel,
//   where it passed behind the surface and is judged by the thickness test.
// - The thickness grows with range: a gap is accepted within the larger of Thickness and
//   StepToleranceScale times the depth one texel of the ray spans where it crossed the surface's
//   depth. Near a ray's vanishing point the texel holding the surface spans every depth from
//   there to infinity, so the span is taken at the crossing, never across that texel.
// - A hit is validated by the frustum, its distance from the origin, the background, the
//   surface's facing and the thickness, under a screen-edge vignette; the confidence is
//   (1 - smoothstep(0, tolerance, gap))^2. A miss writes zero radiance and zero length.
// - The pixels the ray's copy flags name take its result.
//
// The traversal, the hit validation and the quad copies follow the AMD FidelityFX SDK's stochastic
// screen-space reflections, under this licence:
//
// Copyright (C) 2024 Advanced Micro Devices, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
// associated documentation files (the "Software"), to deal in the Software without restriction,
// including without limitation the rights to use, copy, modify, merge, publish, distribute,
// sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all copies or
// substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
// NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT
// OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.
#include "ssr_surface.slang"
#include "Veng/ssr_ray_list.slang"
#include "Veng/ssr_sample.slang"

[[vk::binding(0, 0)]] Texture2D g_Textures[];

// Set 0 is reserved for the bindless registry; the chain's lists and storage targets bind on set 3.
[[vk::binding(0, 3)]] StructuredBuffer<uint> g_RayList;
[[vk::binding(1, 3)]] StructuredBuffer<uint> g_Counters;
[[vk::binding(2, 3)]] [format("rgba16f")] RWTexture2D<float4> g_Radiance;
[[vk::binding(3, 3)]] [format("r16f")] RWTexture2D<float> g_RayLength;

struct PushConstants
{
    uint PyramidTexture;      // nearest-depth pyramid, level 0 the depth at render resolution
    uint NormalTexture;       // G1 world-space normal
    uint AlphaTexture;        // the classifier's extracted GGX α
    uint BlueNoiseTexture;    // the 2D lobe-sample tile
    uint SceneTexture;        // the lit scene the rays reflect
    uint ViewConstantsIndex;  // current frame-in-flight's view-constants region
    uint PyramidLevels;       // the pyramid's level count
    uint MaxIterations;       // the traversal budget per ray
    uint2 Extent;             // render extent in pixels
    float Thickness;          // view-space depth gap always accepted as a hit
    uint Pad0;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

// The coarsest level the march climbs to.
static const int MaxTraversalMip = 6;

// A crossing's gap may run to this multiple of the depth one texel of the ray spans there.
static const float StepToleranceScale = 1.5;

// A cell boundary is crossed this far into the next cell, in level-0 texels, so the next iteration
// reads the next cell rather than the one just left.
static const float CellExitOvershoot = 0.005;

// The march stops once no more than this share of its subgroup's lanes still run: the stragglers
// would hold the whole subgroup. A mirror never stops early.
static const uint MinOccupancyDivisor = 8u;

// A hit closer to the origin than this, in pixels along both axes, is the ray's own surface.
static const float MinHitPixels = 2.0;

// The screen-edge fade's width, as a share of the height; the horizontal one is scaled by aspect.
static const float VignetteShare = 0.05;

// The largest ray length the R16F target stores.
static const float MaxStoredRayLength = 65504.0;

static const float FloatMax = 3.402823466e+38;

// The view-space position of screen UV `uv` at device depth `depth`, camera-relative.
float3 ViewPositionAt(ViewConstants vc, float2 uv, float depth)
{
    return mul((float3x3)vc.View, RelativePosition(vc, uv, depth));
}

// The view-space depth (distance in front of the camera) of a screen-space point.
float ViewDepthAt(ViewConstants vc, float3 screen)
{
    return -ViewPositionAt(vc, screen.xy, screen.z).z;
}

// A view-space position projected to screen UV and device depth.
float3 ProjectPosition(ViewConstants vc, float3 viewPos)
{
    float4 clip = mul(vc.Proj, float4(viewPos, 1.0));
    float3 ndc = clip.xyz / clip.w;
    return float3(ndc.xy * 0.5 + 0.5, ndc.z);
}

// The pyramid's nearest depth over a cell of `level`, read clamped into the level.
float LoadDepth(Texture2D pyramid, int2 cell, int level)
{
    int2 size = max(int2(g_PC.Extent) >> level, int2(1, 1));
    return pyramid.Load(int3(clamp(cell, int2(0, 0), size - 1), level)).r;
}

// Advances the ray from its origin to the boundary of the cell it starts in, so it does not meet
// its own surface first.
void InitialAdvanceRay(float3 origin, float3 direction, float3 invDirection, float2 mipResolution,
                       float2 mipResolutionInv, float2 floorOffset, float2 uvOffset,
                       out float3 position, out float t)
{
    float2 mipPosition = mipResolution * origin.xy;
    // The half of the cell's box pointing away from the origin.
    float2 xyPlane = floor(mipPosition) + floorOffset;
    xyPlane = xyPlane * mipResolutionInv + uvOffset;
    float2 tPlane = xyPlane * invDirection.xy - origin.xy * invDirection.xy;
    t = min(tPlane.x, tPlane.y);
    position = origin + t * direction;
}

// Advances the ray to the nearest of its cell's x and y planes and the cell's depth plane, unless it
// is already behind the cell's surface. Returns whether it left the cell above the surface.
bool AdvanceRay(float3 origin, float3 direction, float3 invDirection, float2 mipPosition,
                float2 mipResolutionInv, float2 floorOffset, float2 uvOffset, float surfaceZ,
                inout float3 position, inout float t)
{
    float2 xyPlane = floor(mipPosition) + floorOffset;
    xyPlane = xyPlane * mipResolutionInv + uvOffset;
    float3 boundaryPlanes = float3(xyPlane, surfaceZ);
    float3 tPlane = boundaryPlanes * invDirection - origin * invDirection;
    // Reverse-Z: the depth plane only bounds a ray heading away from the camera.
    tPlane.z = direction.z < 0.0 ? tPlane.z : FloatMax;
    float tMin = min(min(tPlane.x, tPlane.y), tPlane.z);
    // Reverse-Z: a larger depth is nearer the camera.
    bool aboveSurface = surfaceZ < position.z;
    // Bitwise, so a NaN or an infinity cannot make the depth plane look like an xy plane.
    bool skippedCell = asuint(tMin) != asuint(tPlane.z) && aboveSurface;
    t = aboveSurface ? tMin : t;
    position = origin + t * direction;
    return skippedCell;
}

// Where a march stopped.
struct March
{
    float3 Position;  // screen UV and device depth
    float T;          // the ray parameter there
    bool Met;         // it descended below level 0: the ray met a surface
};

// Marches the screen-space ray from `origin` along `direction` through the pyramid.
March HierarchicalRaymarch(Texture2D pyramid, float3 origin, float3 direction, bool isMirror,
                           float2 screenSize, int maxMip, uint maxIterations)
{
    float3 invDirection = float3(direction.x != 0.0 ? 1.0 / direction.x : FloatMax,
                                 direction.y != 0.0 ? 1.0 / direction.y : FloatMax,
                                 direction.z != 0.0 ? 1.0 / direction.z : FloatMax);

    int mip = 0;
    float2 mipResolution = screenSize;
    float2 mipResolutionInv = 1.0 / mipResolution;

    float2 uvOffset = CellExitOvershoot / screenSize;
    uvOffset.x = direction.x < 0.0 ? -uvOffset.x : uvOffset.x;
    uvOffset.y = direction.y < 0.0 ? -uvOffset.y : uvOffset.y;
    // Moves each boundary plane to the cell's far side along the ray.
    float2 floorOffset = float2(direction.x < 0.0 ? 0.0 : 1.0, direction.y < 0.0 ? 0.0 : 1.0);

    March march;
    InitialAdvanceRay(origin, direction, invDirection, mipResolution, mipResolutionInv, floorOffset,
                      uvOffset, march.Position, march.T);

    uint minOccupancy = WaveGetLaneCount() / MinOccupancyDivisor;
    bool lowOccupancy = false;
    uint i = 0u;
    while (i < maxIterations && mip >= 0 && !lowOccupancy && all(march.Position.xy >= 0.0) &&
           all(march.Position.xy <= 1.0))
    {
        float2 mipPosition = mipResolution * march.Position.xy;
        float surfaceZ = LoadDepth(pyramid, int2(mipPosition), mip);
        lowOccupancy = !isMirror && WaveActiveCountBits(true) <= minOccupancy;
        bool skippedCell = AdvanceRay(origin, direction, invDirection, mipPosition, mipResolutionInv,
                                      floorOffset, uvOffset, surfaceZ, march.Position, march.T);
        // The pyramid holds no level above maxMip.
        if (!(skippedCell && mip >= maxMip))
        {
            mip += skippedCell ? 1 : -1;
            mipResolution *= skippedCell ? 0.5 : 2.0;
            mipResolutionInv *= skippedCell ? 2.0 : 0.5;
        }
        ++i;
    }
    march.Met = mip < 0;
    return march;
}

// The depth one level-0 texel of the ray spans ending at ray parameter `t`, in view space.
float TexelDepthSpan(ViewConstants vc, float3 origin, float3 direction, float texelStep, float t)
{
    float before = max(t - texelStep, 0.0);
    return abs(ViewDepthAt(vc, origin + direction * t) - ViewDepthAt(vc, origin + direction * before));
}

[shader("compute")]
[numthreads(64, 1, 1)]
void csMain(uint localIndex : SV_GroupIndex, uint3 group : SV_GroupID)
{
    uint rayIndex = group.x * 64u + localIndex;
    if (rayIndex >= g_Counters[SsrRayCountCounter])
        return;

    SsrRay ray = SsrUnpackRay(g_RayList[rayIndex]);
    int2 pixel = int2(ray.Pixel);
    float2 screenSize = float2(g_PC.Extent);
    float2 uv = (float2(pixel) + 0.5) / screenSize;

    ViewConstants vc = LoadViewConstants(g_PC.ViewConstantsIndex);
    Texture2D pyramid = g_Textures[g_PC.PyramidTexture];
    Texture2D normalTexture = g_Textures[g_PC.NormalTexture];

    float3 worldNormal = normalize(normalTexture.Load(int3(pixel, 0)).xyz);
    float alpha = g_Textures[g_PC.AlphaTexture].Load(int3(pixel, 0)).r;
    bool isMirror = alpha < SsrMirrorAlpha;

    float originDepth = pyramid.Load(int3(pixel, 0)).r;
    float3 origin = float3(uv, originDepth);
    float3 viewOrigin = ViewPositionAt(vc, uv, originDepth);
    // Under perspective the camera is the view-space origin, so the point is its own view ray;
    // under an orthographic projection every view ray runs down the view axis.
    bool orthographic = vc.Proj[3][3] > 0.5;
    float3 viewRay = orthographic ? float3(0.0, 0.0, -1.0) : normalize(viewOrigin);
    float3 viewNormal = mul((float3x3)vc.View, worldNormal);
    float2 u = SsrBlueNoise(g_Textures[g_PC.BlueNoiseTexture], ray.Pixel, vc.FrameIndex);
    float3 reflected = SampleReflectionVector(viewRay, viewNormal, alpha, u);

    // The screen-space direction: the projection of a point along the reflected ray. Any point in
    // front of the camera gives the same line; one as far along as the origin is from the eye keeps
    // it well conditioned, pulled in for a ray heading back toward the camera so it stays in front.
    float reach = orthographic ? 1.0 : max(length(viewOrigin), 1e-4);
    if (!orthographic && reflected.z > 0.0)
        reach = min(reach, 0.5 * -viewOrigin.z / reflected.z);
    float3 direction = ProjectPosition(vc, viewOrigin + reflected * reach) - origin;

    int maxMip = min(MaxTraversalMip, int(max(g_PC.PyramidLevels, 1u)) - 1);
    March march = HierarchicalRaymarch(pyramid, origin, direction, isMirror, screenSize, maxMip,
                                       g_PC.MaxIterations);

    float confidence = 0.0;
    float3 hit = march.Position;
    int2 hitTexel = clamp(int2(hit.xy * screenSize), int2(0, 0), int2(g_PC.Extent) - 1);
    if (march.Met && all(hit.xy >= 0.0) && all(hit.xy <= 1.0))
    {
        float surfaceZ = pyramid.Load(int3(hitTexel, 0)).r;

        // The ray stopped behind the surface it stands over: if it crossed that surface's depth
        // inside the texel — past the cell boundary it overshot onto — the hit is that crossing.
        float crossing = direction.z != 0.0 ? (surfaceZ - origin.z) / direction.z : FloatMax;
        if (hit.z < surfaceZ && direction.z < 0.0)
        {
            float2 nearPlane = (float2(hitTexel) + float2(direction.x < 0.0 ? 1.0 : 0.0,
                                                          direction.y < 0.0 ? 1.0 : 0.0)) /
                               screenSize;
            float2 tPlane = (nearPlane - origin.xy) / direction.xy;
            float entry = max(direction.x != 0.0 ? tPlane.x : -FloatMax,
                              direction.y != 0.0 ? tPlane.y : -FloatMax);
            if (crossing >= entry)
            {
                march.T = crossing;
                hit = float3(origin.xy + direction.xy * crossing, surfaceZ);
            }
        }

        float2 fromOrigin = abs(hit.xy - uv) * screenSize;
        bool selfHit = fromOrigin.x < MinHitPixels && fromOrigin.y < MinHitPixels;
        int coarseLevel = min(1, maxMip);
        bool background = LoadDepth(pyramid, hitTexel >> coarseLevel, coarseLevel) <= 0.0 || surfaceZ <= 0.0;
        float3 hitWorldNormal = normalTexture.Load(int3(hitTexel, 0)).xyz;
        float3 worldRay = RelativePosition(vc, hit.xy, hit.z) - RelativePosition(vc, uv, originDepth);
        bool backFace = dot(hitWorldNormal, worldRay) > 0.0;
        if (!selfHit && !background && !backFace)
        {
            // One level-0 texel along the ray's longer screen axis, in ray parameter.
            float texelStep = 1.0 / max(max(abs(direction.x) * screenSize.x, abs(direction.y) * screenSize.y),
                                        1e-20);
            float spanAt = min(max(min(crossing, march.T), texelStep), march.T);
            float span = TexelDepthSpan(vc, origin, direction, texelStep, spanAt);
            float tolerance = max(g_PC.Thickness, StepToleranceScale * span);
            float gap = abs(ViewDepthAt(vc, float3(hit.xy, surfaceZ)) - ViewDepthAt(vc, hit));
            float thickness = 1.0 - smoothstep(0.0, tolerance, gap);

            float2 fade = VignetteShare * float2(screenSize.y / screenSize.x, 1.0);
            float2 border = smoothstep(float2(0.0, 0.0), fade, hit.xy) *
                            (1.0 - smoothstep(float2(1.0, 1.0) - fade, float2(1.0, 1.0), hit.xy));
            confidence = border.x * border.y * thickness * thickness;
        }
    }

    float4 radiance = float4(0.0, 0.0, 0.0, 0.0);
    float rayLength = 0.0;
    if (confidence > 0.0)
    {
        float3 colour = g_Textures[g_PC.SceneTexture].Load(int3(hitTexel, 0)).rgb;
        radiance = float4(colour * confidence, confidence);
        float3 worldRay = RelativePosition(vc, hit.xy, hit.z) - RelativePosition(vc, uv, originDepth);
        rayLength = min(length(worldRay), MaxStoredRayLength);
    }

    g_Radiance[pixel] = radiance;
    g_RayLength[pixel] = rayLength;
    int2 across = pixel ^ int2(1, 1);
    if (ray.CopyHorizontal)
    {
        g_Radiance[int2(across.x, pixel.y)] = radiance;
        g_RayLength[int2(across.x, pixel.y)] = rayLength;
    }
    if (ray.CopyVertical)
    {
        g_Radiance[int2(pixel.x, across.y)] = radiance;
        g_RayLength[int2(pixel.x, across.y)] = rayLength;
    }
    if (ray.CopyDiagonal)
    {
        g_Radiance[across] = radiance;
        g_RayLength[across] = rayLength;
    }
}
