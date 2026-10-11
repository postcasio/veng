// Stochastic SSR denoiser, first pass: finds each glossy pixel's reflection in last frame's
// denoised output, decides how many frames of it to keep, and averages each tile's radiance. One
// 8x8 workgroup per listed tile, dispatched indirectly by the tile count.
//
// - Two history positions. The surface's: the pixel's own motion, `uv - motion`, with the jitter
//   change added back, `(previous jitter - this jitter) / 2`, since the motion is unjittered and both
//   frames stand in their jittered rasters. The reflection's: the virtual point a mirror shows the
//   hit at, the pixel's camera-relative position carried a further ray length along its own view
//   ray, projected into last frame through PrevFromCurDirection and the camera's move.
// - Whether a history exists is decided by geometry, never by colour. Last frame's normal and view
//   depth at the surface's position must match the pixel's (the disocclusion factor, searched over
//   the 3x3 texels around it and then rebuilt from the matching texels of its 2x2 footprint), and
//   the hit must not have jumped: last frame's ray lengths there and this frame's around the pixel
//   must overlap within a factor of RayLengthJump. Failing either, the pixel starts again.
// - The history is read at the virtual point where last frame's normal there agrees with the
//   pixel's past 0.9999, at least as well as the surface's, and its alpha is as close (a planar
//   reflector, which the virtual point reprojects exactly); otherwise at the surface's. The read is
//   Catmull-Rom (catmull_rom.slang), so thin, bright reflected content stays sharp where a bilinear
//   read every frame would blur it.
// - The sample count grows by one a frame up to a cap set by how far the history can be trusted:
//   max(8, 32 (1 - exp(-100 alpha))) for a still reflection, shortened by how far the reflected
//   image moved this frame (which the Catmull-Rom read resamples) and, more steeply, by how far from
//   the virtual point the history was read (the surface's position under a camera's translation,
//   which shows other reflected content), and never below MovingHistorySamples.
// - Without history the pixel starts again: no reprojected radiance, a variance of 1 (so the
//   classifier traces it next frame) and one sample. With it, the variance is the temporal luminance
//   variance mixed into the history's by 1 / samples.
// - The tile's average, weighed by exp(-0.3 luminance) over its glossy pixels, is this frame's
//   radiance blended 30 % toward the reprojected history.
//
// The denoiser follows the AMD FidelityFX SDK's reflections denoiser, under this licence:
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
#include "catmull_rom.slang"
#include "ssr_denoise.slang"

[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];

// Set 0 is reserved for the bindless registry; the tile list and the pass's targets bind on set 3.
[[vk::binding(0, 3)]] StructuredBuffer<uint> g_TileList;
[[vk::binding(1, 3)]] [format("rgba16f")] RWTexture2D<float4> g_Reprojected;
[[vk::binding(2, 3)]] [format("r16f")] RWTexture2D<float> g_Variance;
[[vk::binding(3, 3)]] [format("r16f")] RWTexture2D<float> g_SampleCount;
[[vk::binding(4, 3)]] [format("rgba16f")] RWTexture2D<float4> g_Average;

struct PushConstants
{
    uint RadianceTexture;            // this frame's intersection, premultiplied
    uint RayLengthTexture;           // its world-space ray lengths
    uint AlphaTexture;               // the classifier's GGX alpha
    uint NormalTexture;              // G1 world-space normal
    uint DepthTexture;               // the nearest-depth pyramid, level 0 the depth
    uint VelocityTexture;            // G3 unjittered motion, current UV minus previous
    uint RadianceHistoryTexture;     // last frame's denoised output
    uint VarianceHistoryTexture;     // last frame's resolved variance
    uint SampleCountHistoryTexture;  // last frame's sample counts
    uint NormalHistoryTexture;       // last frame's G1
    uint AlphaHistoryTexture;        // last frame's alpha
    uint DepthHistoryTexture;        // last frame's depth (R32F, read by loads)
    uint Sampler;                    // bilinear, clamped to the edge
    uint ViewConstantsIndex;         // current frame-in-flight's view-constants region
    uint2 Extent;                    // render extent in pixels
    float MaxAlpha;                  // glossy below this alpha
    uint HistoryValid;               // 0 on the first frame after the targets were created
    uint RayLengthHistoryTexture;    // last frame's ray lengths (R16F, read by loads)
    uint Pad0;
    // The previous unjittered (projection x rotation-only view) applied to the camera's move since
    // then, (camera - previous camera, 0): the clip offset a camera-relative point takes between the
    // two frames beyond what PrevFromCurDirection carries.
    float4 PrevClipTranslation;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

// The tile average's reduction: luminance-weighted radiance, and the weights.
groupshared float4 g_WeightedSum[8][8];
groupshared float g_WeightSum[8][8];

bool LoadGlossy(int2 pixel)
{
    if (!InsideExtent(pixel, g_PC.Extent))
        return false;
    return IsGlossy(g_Textures[g_PC.AlphaTexture].Load(int3(pixel, 0)).r, g_PC.MaxAlpha);
}

float3 LoadNormal(int2 pixel)
{
    return SafeNormalize(g_Textures[g_PC.NormalTexture].Load(int3(pixel, 0)).xyz);
}

float4 SampleRadianceHistory(float2 uv)
{
    return SampleCatmullRom(g_Textures[g_PC.RadianceHistoryTexture], g_Samplers[g_PC.Sampler], uv,
                            float2(g_PC.Extent));
}

float4 LoadRadianceHistory(int2 pixel)
{
    if (!InsideExtent(pixel, g_PC.Extent))
        return float4(0.0, 0.0, 0.0, 0.0);
    return g_Textures[g_PC.RadianceHistoryTexture].Load(int3(pixel, 0));
}

float3 SampleNormalHistory(float2 uv)
{
    return SafeNormalize(
        g_Textures[g_PC.NormalHistoryTexture].SampleLevel(g_Samplers[g_PC.Sampler], uv, 0.0).xyz);
}

float3 LoadNormalHistory(int2 pixel)
{
    if (!InsideExtent(pixel, g_PC.Extent))
        return float3(0.0, 0.0, 0.0);
    return SafeNormalize(g_Textures[g_PC.NormalHistoryTexture].Load(int3(pixel, 0)).xyz);
}

float SampleAlphaHistory(float2 uv)
{
    return g_Textures[g_PC.AlphaHistoryTexture].SampleLevel(g_Samplers[g_PC.Sampler], uv, 0.0).r;
}

float SampleVarianceHistory(float2 uv)
{
    return g_Textures[g_PC.VarianceHistoryTexture].SampleLevel(g_Samplers[g_PC.Sampler], uv, 0.0).r;
}

float SampleSampleCountHistory(float2 uv)
{
    return g_Textures[g_PC.SampleCountHistoryTexture]
        .SampleLevel(g_Samplers[g_PC.Sampler], uv, 0.0)
        .r;
}

float LoadDepthHistory(int2 pixel)
{
    if (!InsideExtent(pixel, g_PC.Extent))
        return 0.0;
    return g_Textures[g_PC.DepthHistoryTexture].Load(int3(pixel, 0)).r;
}

// The depth history read bilinearly by hand: a 32-bit float target is not promised filterable.
float SampleDepthHistory(float2 uv)
{
    float2 position = uv * float2(g_PC.Extent) - 0.5;
    int2 base = int2(floor(position));
    float2 f = position - float2(base);
    int2 last = int2(g_PC.Extent) - 1;
    float d00 = LoadDepthHistory(clamp(base, int2(0, 0), last));
    float d10 = LoadDepthHistory(clamp(base + int2(1, 0), int2(0, 0), last));
    float d01 = LoadDepthHistory(clamp(base + int2(0, 1), int2(0, 0), last));
    float d11 = LoadDepthHistory(clamp(base + int2(1, 1), int2(0, 0), last));
    return lerp(lerp(d00, d10, f.x), lerp(d01, d11, f.x), f.y);
}

// The jittered last-frame UV of a camera-relative point that does not move in the world: its
// unjittered projection this frame, carried into last frame through PrevFromCurDirection and the
// camera's move, then offset by last frame's jitter.
float2 PreviousUV(ViewConstants vc, float3 relative)
{
    float4 current = mul(vc.Proj, float4(mul((float3x3)vc.View, relative), 1.0));
    current.xy -= vc.JitterNdc.xy * current.w;
    float4 previous = mul(vc.PrevFromCurDirection, current) + g_PC.PrevClipTranslation;
    if (previous.w <= 1e-6)
        return float2(-1.0, -1.0);
    return (previous.xy / previous.w) * 0.5 + 0.5 + vc.JitterNdc.zw * 0.5;
}

// The surface's own history position: the unjittered motion taken back out, with the change of
// jitter between the frames added.
float2 SurfaceReprojection(ViewConstants vc, int2 pixel, float2 uv)
{
    float2 motion = g_Textures[g_PC.VelocityTexture].Load(int3(pixel, 0)).xy;
    return uv - motion + (vc.JitterNdc.zw - vc.JitterNdc.xy) * 0.5;
}

// The history position of the virtual point a mirror shows the hit at: the pixel carried
// `rayLength` further along its own view ray, through the surface.
float2 HitReprojection(ViewConstants vc, float2 uv, float depth, float rayLength)
{
    float3 relative = RelativePosition(vc, uv, depth);
    // Under an orthographic projection every view ray runs down the view axis.
    bool orthographic = vc.Proj[3][3] > 0.5;
    float3 virtualPoint;
    if (orthographic)
    {
        virtualPoint = relative + mul(float3(0.0, 0.0, -1.0), (float3x3)vc.View) * rayLength;
    }
    else
    {
        float surfaceDistance = max(length(relative), 1e-20);
        virtualPoint = relative * ((surfaceDistance + rayLength) / surfaceDistance);
    }
    return PreviousUV(vc, virtualPoint);
}

float DisocclusionFactor(float3 normal, float3 historyNormal, float linearDepth,
                         float historyLinearDepth)
{
    return exp(-abs(1.0 - max(0.0, dot(normal, historyNormal))) * DisocclusionNormalWeight) *
           exp(-abs(historyLinearDepth - linearDepth) / linearDepth * DisocclusionDepthWeight);
}

bool OnScreen(float2 uv)
{
    return all(uv > 0.0) && all(uv < 1.0);
}

// Whether the hit jumped between the frames: last frame's ray lengths over the 2x2 footprint of
// `historyUv` and this frame's over the pixel's 3x3 (which spans several rays where a quad shares
// one) do not overlap within RayLengthJump. A miss carries no length, so a side holding only misses
// has nothing to compare, and the colour clip judges it instead.
bool HitJumped(int2 pixel, float2 historyUv)
{
    int2 last = int2(g_PC.Extent) - 1;
    float currentNearest = 1e30;
    float currentFarthest = 0.0;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            int2 texel = clamp(pixel + int2(x, y), int2(0, 0), last);
            float hitLength = g_Textures[g_PC.RayLengthTexture].Load(int3(texel, 0)).r;
            if (hitLength > 0.0)
            {
                currentNearest = min(currentNearest, hitLength);
                currentFarthest = max(currentFarthest, hitLength);
            }
        }
    }
    float historyNearest = 1e30;
    float historyFarthest = 0.0;
    int2 base = int2(floor(historyUv * float2(g_PC.Extent) - 0.5));
    for (int k = 0; k < 4; ++k)
    {
        int2 texel = clamp(base + int2(k & 1, k >> 1), int2(0, 0), last);
        float hitLength = g_Textures[g_PC.RayLengthHistoryTexture].Load(int3(texel, 0)).r;
        if (hitLength > 0.0)
        {
            historyNearest = min(historyNearest, hitLength);
            historyFarthest = max(historyFarthest, hitLength);
        }
    }
    if (currentFarthest <= 0.0 || historyFarthest <= 0.0)
        return false;
    return historyNearest > currentFarthest * RayLengthJump ||
           historyFarthest * RayLengthJump < currentNearest;
}

struct Reprojection
{
    bool Valid;           // a history exists
    float Disocclusion;   // the surface's disocclusion factor, at least DisocclusionThreshold
    float2 Uv;            // where the history was read
    float4 Radiance;      // the history read there
    float MotionPixels;   // how far the reflected image moved since last frame, in pixels
    float MisplacedPixels; // how far from the reflection's last position the history was read
};

Reprojection Reproject(ViewConstants vc, int2 pixel, float2 uv, float alpha, float rayLength,
                       float3 normal, float depth, float linearDepth)
{
    Reprojection result;
    result.Valid = false;
    result.Disocclusion = 0.0;
    result.Uv = float2(-1.0, -1.0);
    result.Radiance = float4(0.0, 0.0, 0.0, 0.0);
    result.MotionPixels = 0.0;
    result.MisplacedPixels = 0.0;

    float2 surfaceUv = SurfaceReprojection(vc, pixel, uv);
    float2 hitUv = HitReprojection(vc, uv, depth, rayLength);

    // Disocclusion is the surface's: was this pixel's surface where its motion says, last frame?
    float factor = DisocclusionFactor(normal, SampleNormalHistory(surfaceUv), linearDepth,
                                      LinearDepth(vc, surfaceUv, SampleDepthHistory(surfaceUv)));
    if (factor <= DisocclusionThreshold)
    {
        // Not convinced of a disocclusion: search the texels around it for a closer match.
        float2 dudv = 1.0 / float2(g_PC.Extent);
        float2 centre = surfaceUv;
        for (int y = -1; y <= 1; y++)
        {
            for (int x = -1; x <= 1; x++)
            {
                float2 candidateUv = centre + float2(x, y) * dudv;
                float weight = DisocclusionFactor(
                    normal, SampleNormalHistory(candidateUv), linearDepth,
                    LinearDepth(vc, candidateUv, SampleDepthHistory(candidateUv)));
                if (weight > factor)
                {
                    factor = weight;
                    surfaceUv = candidateUv;
                }
            }
        }
    }

    // Still short, typically at an edge in motion: rebuild the history from those of the 2x2
    // texels whose surface matches, weighted bilinearly.
    bool rebuilt = false;
    float4 rebuiltRadiance = float4(0.0, 0.0, 0.0, 0.0);
    if (factor < DisocclusionThreshold)
    {
        float2 position = float2(g_PC.Extent) * surfaceUv - 0.5;
        float2 f = frac(position + 1.0);
        int2 base = int2(floor(position));
        int2 offsets[4] = {int2(0, 0), int2(1, 0), int2(0, 1), int2(1, 1)};
        float bilinear[4] = {(1.0 - f.x) * (1.0 - f.y), f.x * (1.0 - f.y), (1.0 - f.x) * f.y,
                             f.x * f.y};
        float4 radiance[4];
        float3 normals[4];
        float depths[4];
        float weights[4];
        float weightSum = 0.0;
        for (int k = 0; k < 4; ++k)
        {
            int2 texel = base + offsets[k];
            radiance[k] = LoadRadianceHistory(texel);
            normals[k] = LoadNormalHistory(texel);
            depths[k] = LinearDepth(vc, surfaceUv, LoadDepthHistory(texel));
            float matches = DisocclusionFactor(normal, normals[k], linearDepth, depths[k]) >
                                    DisocclusionThreshold / 2.0
                                ? 1.0
                                : 0.0;
            weights[k] = matches * bilinear[k];
            weightSum += weights[k];
        }
        weightSum = max(weightSum, 1.0e-3);
        float3 rebuiltNormal = float3(0.0, 0.0, 0.0);
        float rebuiltDepth = 0.0;
        for (int k = 0; k < 4; ++k)
        {
            float weight = weights[k] / weightSum;
            rebuiltRadiance += radiance[k] * weight;
            rebuiltNormal += normals[k] * weight;
            rebuiltDepth += depths[k] * weight;
        }
        factor = DisocclusionFactor(normal, rebuiltNormal, linearDepth, rebuiltDepth);
        rebuilt = true;
    }
    if (factor < DisocclusionThreshold || !OnScreen(surfaceUv) || HitJumped(pixel, surfaceUv))
        return result;

    // The reflection's own position for a planar reflector, else the surface's.
    float3 hitNormal = SampleNormalHistory(hitUv);
    float hitSimilarity = dot(hitNormal, normal);
    float surfaceSimilarity = dot(SampleNormalHistory(surfaceUv), normal);
    bool atHit = OnScreen(hitUv) && hitSimilarity > ReprojectionNormalSimilarityThreshold &&
                 hitSimilarity + 1.0e-3 > surfaceSimilarity &&
                 abs(SampleAlphaHistory(hitUv) - alpha) <
                     abs(SampleAlphaHistory(surfaceUv) - alpha) + 1.0e-3;

    result.Valid = true;
    result.Disocclusion = factor;
    result.Uv = atHit ? hitUv : surfaceUv;
    result.Radiance = atHit    ? SampleRadianceHistory(hitUv)
                      : rebuilt ? rebuiltRadiance
                                : SampleRadianceHistory(surfaceUv);
    float2 extent = float2(g_PC.Extent);
    float2 jitterChange = (vc.JitterNdc.zw - vc.JitterNdc.xy) * 0.5;
    result.MotionPixels = length((hitUv - uv - jitterChange) * extent);
    result.MisplacedPixels = length((result.Uv - hitUv) * extent);
    return result;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint2 groupThread : SV_GroupThreadID, uint3 group : SV_GroupID)
{
    int2 origin = DenoiseTileOrigin(g_TileList, group.x);
    int2 thread = int2(groupThread);
    int2 pixel = origin + thread;

    bool inside = InsideExtent(pixel, g_PC.Extent);
    bool glossy = LoadGlossy(pixel);
    float4 radiance = glossy ? g_Textures[g_PC.RadianceTexture].Load(int3(pixel, 0))
                             : float4(0.0, 0.0, 0.0, 0.0);

    if (glossy)
    {
        ViewConstants vc = LoadViewConstants(g_PC.ViewConstantsIndex);
        float2 uv = (float2(pixel) + 0.5) / float2(g_PC.Extent);
        float alpha = g_Textures[g_PC.AlphaTexture].Load(int3(pixel, 0)).r;
        float3 normal = LoadNormal(pixel);
        float depth = g_Textures[g_PC.DepthTexture].Load(int3(pixel, 0)).r;
        float rayLength = g_Textures[g_PC.RayLengthTexture].Load(int3(pixel, 0)).r;
        float linearDepth = LinearDepth(vc, uv, depth);

        Reprojection reprojection;
        reprojection.Valid = false;
        if (g_PC.HistoryValid != 0u)
            reprojection =
                Reproject(vc, pixel, uv, alpha, rayLength, normal, depth, linearDepth);

        if (reprojection.Valid)
        {
            float previousVariance = SampleVarianceHistory(reprojection.Uv);
            float stillSamples =
                max(MinHistorySamples, MaxHistorySamples * SamplesForRoughness(alpha));
            float trustedSamples = max(
                MovingHistorySamples,
                stillSamples / (1.0 + reprojection.MotionPixels / HistoryMotionPixels +
                                reprojection.MisplacedPixels / HistoryMisplacementPixels));
            float samples = SampleSampleCountHistory(reprojection.Uv) * reprojection.Disocclusion;
            samples = min(min(stillSamples, trustedSamples), samples + 1.0);
            float newVariance = TemporalVariance(radiance.rgb, reprojection.Radiance.rgb);
            g_Reprojected[pixel] = reprojection.Radiance;
            g_Variance[pixel] = lerp(newVariance, previousVariance, 1.0 / samples);
            g_SampleCount[pixel] = samples;
            // The average sees this frame's radiance part-way to the history.
            radiance = lerp(radiance, reprojection.Radiance, 0.3);
        }
        else
        {
            g_Reprojected[pixel] = float4(0.0, 0.0, 0.0, 0.0);
            g_Variance[pixel] = 1.0;
            g_SampleCount[pixel] = 1.0;
        }
    }

    // The tile's average radiance, weighed against fireflies, over its glossy pixels.
    float weight = glossy ? max(exp(-DenoiseLuminance(radiance.rgb) * AverageRadianceLuminanceWeight),
                                1.0e-2)
                          : 0.0;
    float4 weighted = radiance * weight;
    if (!inside || any(isinf(weighted)) || any(isnan(weighted)) || weight > 1.0e3)
    {
        weighted = float4(0.0, 0.0, 0.0, 0.0);
        weight = 0.0;
    }
    g_WeightedSum[thread.y][thread.x] = weighted;
    g_WeightSum[thread.y][thread.x] = weight;
    GroupMemoryBarrierWithGroupSync();

    for (int step = 2; step <= 8; step *= 2)
    {
        int2 lower = thread * step;
        int2 upper = lower + step / 2;
        if (upper.x < 8 && upper.y < 8)
        {
            g_WeightedSum[lower.y][lower.x] =
                g_WeightedSum[lower.y][lower.x] + g_WeightedSum[lower.y][upper.x] +
                g_WeightedSum[upper.y][lower.x] + g_WeightedSum[upper.y][upper.x];
            g_WeightSum[lower.y][lower.x] = g_WeightSum[lower.y][lower.x] +
                                            g_WeightSum[lower.y][upper.x] +
                                            g_WeightSum[upper.y][lower.x] + g_WeightSum[upper.y][upper.x];
        }
        GroupMemoryBarrierWithGroupSync();
    }

    if (all(thread == int2(0, 0)))
        g_Average[origin / 8] = g_WeightedSum[0][0] / max(g_WeightSum[0][0], 1.0e-3);
}
