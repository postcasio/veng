// Stochastic SSR denoiser, second pass: spreads each noisy glossy pixel's reflection over its
// neighbours that share its surface. One 8x8 workgroup per listed tile, dispatched indirectly by
// the tile count.
//
// - A pixel with no variance, a mirror and a pixel that is not glossy pass through unfiltered.
// - Otherwise the result is the pixel and fifteen Halton(2, 3) taps within three texels of it,
//   each weighed by its normal's agreement, pow(n·n', 512), its view depth's, exp(-|Δz| z 4), its
//   radiance's distance from the tile average, exp(-0.1 variance |avg - L|) floored at 0.01 (see
//   RadianceWeightBias), and the pixel's variance, max(0.1, 1 - exp(-4.4 variance)). The pixel
//   itself is weighed by its radiance weight alone. The variance accumulates with the squared
//   weights.
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
#include "ssr_denoise.slang"

[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];

// Set 0 is reserved for the bindless registry; the tile list and the pass's targets bind on set 3.
[[vk::binding(0, 3)]] StructuredBuffer<uint> g_TileList;
[[vk::binding(1, 3)]] [format("rgba16f")] RWTexture2D<float4> g_Prefiltered;
[[vk::binding(2, 3)]] [format("r16f")] RWTexture2D<float> g_PrefilteredVariance;

struct PushConstants
{
    uint RadianceTexture;      // this frame's intersection, premultiplied
    uint VarianceTexture;      // the reprojection's variance
    uint AlphaTexture;         // the classifier's GGX alpha
    uint NormalTexture;        // G1 world-space normal
    uint DepthTexture;         // the nearest-depth pyramid, level 0 the depth
    uint AverageTexture;       // the reprojection's tile averages
    uint Sampler;              // bilinear, clamped to the edge
    uint ViewConstantsIndex;   // current frame-in-flight's view-constants region
    uint2 Extent;              // render extent in pixels
    float MaxAlpha;            // glossy below this alpha
    uint Pad0;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

// The 16x16 neighbourhood: radiance; normal and variance; view depth and whether it is glossy.
groupshared float4 g_Radiance[16][16];
groupshared float4 g_NormalVariance[16][16];
groupshared float2 g_DepthGlossy[16][16];

struct Neighbour
{
    float4 Radiance;
    float Variance;
    float3 Normal;
    float Depth;
    float Glossy;
};

Neighbour LoadNeighbour(int2 index)
{
    Neighbour neighbour;
    neighbour.Radiance = g_Radiance[index.y][index.x];
    neighbour.Normal = g_NormalVariance[index.y][index.x].xyz;
    neighbour.Variance = g_NormalVariance[index.y][index.x].w;
    neighbour.Depth = g_DepthGlossy[index.y][index.x].x;
    neighbour.Glossy = g_DepthGlossy[index.y][index.x].y;
    return neighbour;
}

void StageNeighbour(ViewConstants vc, int2 pixel, int2 index)
{
    float4 radiance = float4(0.0, 0.0, 0.0, 0.0);
    float variance = 0.0;
    float3 normal = float3(0.0, 0.0, 0.0);
    float depth = FarDepth;
    float glossy = 0.0;
    if (InsideExtent(pixel, g_PC.Extent))
    {
        int3 texel = int3(pixel, 0);
        radiance = g_Textures[g_PC.RadianceTexture].Load(texel);
        variance = g_Textures[g_PC.VarianceTexture].Load(texel).r;
        normal = SafeNormalize(g_Textures[g_PC.NormalTexture].Load(texel).xyz);
        float2 uv = (float2(pixel) + 0.5) / float2(g_PC.Extent);
        depth = LinearDepth(vc, uv, g_Textures[g_PC.DepthTexture].Load(texel).r);
        glossy = IsGlossy(g_Textures[g_PC.AlphaTexture].Load(texel).r, g_PC.MaxAlpha) ? 1.0 : 0.0;
    }
    g_Radiance[index.y][index.x] = radiance;
    g_NormalVariance[index.y][index.x] = float4(normal, variance);
    g_DepthGlossy[index.y][index.x] = float2(depth, glossy);
}

float NormalWeight(float3 normal, float3 neighbour)
{
    return pow(max(dot(normal, neighbour), 0.0), PrefilterNormalSigma);
}

float DepthWeight(float depth, float neighbour)
{
    return exp(-abs(depth - neighbour) * depth * PrefilterDepthSigma);
}

float RadianceWeight(float4 average, float4 neighbour, float variance)
{
    return max(exp(-(RadianceWeightBias + variance * RadianceWeightVarianceK) *
                   length(average.rgb - neighbour.rgb)),
               1.0e-2);
}

// The first fifteen points of Halton(2, 3) stretched over [-3, 3], the centre left out.
static const int2 SampleOffsets[15] = {int2(0, 1),  int2(-2, 1),  int2(2, -3), int2(-3, 0),
                                       int2(1, 2),  int2(-1, -2), int2(3, 0),  int2(-3, 3),
                                       int2(0, -3), int2(-1, -1), int2(2, 1),  int2(-2, -2),
                                       int2(1, 0),  int2(0, 2),   int2(3, -1)};

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint2 groupThread : SV_GroupThreadID, uint3 group : SV_GroupID)
{
    int2 origin = DenoiseTileOrigin(g_TileList, group.x);
    int2 thread = int2(groupThread);
    int2 pixel = origin + thread;
    ViewConstants vc = LoadViewConstants(g_PC.ViewConstantsIndex);

    // Stage the 16x16 region from four texels behind the tile to four past it.
    for (int k = 0; k < 4; ++k)
    {
        int2 offset = int2(k & 1, k >> 1) * 8;
        StageNeighbour(vc, origin - 4 + thread + offset, thread + offset);
    }
    GroupMemoryBarrierWithGroupSync();

    if (!InsideExtent(pixel, g_PC.Extent))
        return;

    int2 local = thread + 4;
    Neighbour centre = LoadNeighbour(local);
    float alpha = g_Textures[g_PC.AlphaTexture].Load(int3(pixel, 0)).r;
    float4 resolvedRadiance = centre.Radiance;
    float resolvedVariance = centre.Variance;

    bool needsDenoiser = centre.Variance > 0.0 && centre.Glossy > 0.0 && !IsMirror(alpha);
    if (needsDenoiser)
    {
        float4 average = g_Textures[g_PC.AverageTexture].SampleLevel(
            g_Samplers[g_PC.Sampler], TileAverageUV(pixel, g_PC.Extent), 0.0);

        // The pixel's own weight is its radiance weight, which keeps a firefly from dominating.
        float accumulatedWeight = RadianceWeight(average, centre.Radiance, centre.Variance);
        float4 accumulatedRadiance = centre.Radiance * accumulatedWeight;
        float accumulatedVariance = centre.Variance * accumulatedWeight * accumulatedWeight;
        float varianceWeight =
            max(PrefilterVarianceBias, 1.0 - exp(-(centre.Variance * PrefilterVarianceWeight)));
        for (int i = 0; i < 15; ++i)
        {
            Neighbour neighbour = LoadNeighbour(local + SampleOffsets[i]);
            float weight = neighbour.Glossy;
            weight *= NormalWeight(centre.Normal, neighbour.Normal);
            weight *= DepthWeight(centre.Depth, neighbour.Depth);
            weight *= RadianceWeight(average, neighbour.Radiance, centre.Variance);
            weight *= varianceWeight;

            accumulatedWeight += weight;
            accumulatedRadiance += weight * neighbour.Radiance;
            accumulatedVariance += weight * weight * neighbour.Variance;
        }
        resolvedRadiance = accumulatedRadiance / accumulatedWeight;
        resolvedVariance = accumulatedVariance / (accumulatedWeight * accumulatedWeight);
    }

    g_Prefiltered[pixel] = resolvedRadiance;
    g_PrefilteredVariance[pixel] = resolvedVariance;
}
