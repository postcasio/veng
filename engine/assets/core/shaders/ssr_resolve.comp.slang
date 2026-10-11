// Stochastic SSR denoiser, third pass: blends each glossy pixel's filtered reflection into its
// reprojected history and writes the denoised reflection. One 8x8 workgroup per listed tile,
// dispatched indirectly by the tile count.
//
// - A glossy pixel whose filtered radiance is zero still resolves against its history: a miss is a
//   sample of the premultiplied signal, so a reflection hit on few frames accumulates rather than
//   starting again at every miss. A pixel that is not glossy passes through.
// - The history is clipped (Playdead's ClipAABB) to a box about the neighbourhood's 9x9 Gaussian
//   mean moved a fifth of the way to the tile average, of half-size (sigma + |mean - average|) times
//   the stability factor 0.7 times 1.4.
// - This frame's value is first blended toward the tile average by 1 / (samples + 1), so a young
//   history leans on the tile, and clipped to the average plus or minus the same half-size; it is
//   then blended into the clipped history by 1 / samples.
// - The variance is the temporal variance between the result and the clipped history, blended into
//   the filtered variance the same way. A non-finite result or variance is zero.
//
// The variance is resolved in place: each invocation reads only its own pixel's filtered variance.
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
[[vk::binding(1, 3)]] [format("rgba16f")] RWTexture2D<float4> g_Output;
[[vk::binding(2, 3)]] [format("r16f")] RWTexture2D<float> g_Variance;

struct PushConstants
{
    uint PrefilteredTexture;   // the spatial filter's radiance
    uint ReprojectedTexture;   // the reprojection's history radiance
    uint SampleCountTexture;   // the reprojection's sample counts
    uint AverageTexture;       // the reprojection's tile averages
    uint AlphaTexture;         // the classifier's GGX alpha
    uint Sampler;              // bilinear, clamped to the edge
    uint2 Extent;              // render extent in pixels
    float MaxAlpha;            // glossy below this alpha
    uint Pad0;
    uint Pad1;
    uint Pad2;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

// The 16x16 neighbourhood of the filtered radiance, and whether each texel is glossy.
groupshared float4 g_Radiance[16][16];
groupshared float g_Glossy[16][16];

void StageNeighbour(int2 pixel, int2 index)
{
    float4 radiance = float4(0.0, 0.0, 0.0, 0.0);
    float glossy = 0.0;
    if (InsideExtent(pixel, g_PC.Extent))
    {
        radiance = g_Textures[g_PC.PrefilteredTexture].Load(int3(pixel, 0));
        glossy = IsGlossy(g_Textures[g_PC.AlphaTexture].Load(int3(pixel, 0)).r, g_PC.MaxAlpha)
                     ? 1.0
                     : 0.0;
    }
    g_Radiance[index.y][index.x] = radiance;
    g_Glossy[index.y][index.x] = glossy;
}

Moments EstimateNeighbourhood(int2 local)
{
    Moments moments;
    moments.Mean = float4(0.0, 0.0, 0.0, 0.0);
    moments.Variance = float4(0.0, 0.0, 0.0, 0.0);
    float accumulated = 0.0;
    for (int j = -NeighbourhoodRadius; j <= NeighbourhoodRadius; ++j)
    {
        for (int i = -NeighbourhoodRadius; i <= NeighbourhoodRadius; ++i)
        {
            int2 index = local + int2(i, j);
            float4 radiance = g_Radiance[index.y][index.x];
            float weight = NeighbourhoodKernelWeight(float(i)) * NeighbourhoodKernelWeight(float(j)) *
                           g_Glossy[index.y][index.x];
            accumulated += weight;
            moments.Mean += radiance * weight;
            moments.Variance += radiance * radiance * weight;
        }
    }
    accumulated = max(accumulated, 1e-6);
    moments.Mean /= accumulated;
    moments.Variance /= accumulated;
    moments.Variance = abs(moments.Variance - moments.Mean * moments.Mean);
    return moments;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint2 groupThread : SV_GroupThreadID, uint3 group : SV_GroupID)
{
    int2 origin = DenoiseTileOrigin(g_TileList, group.x);
    int2 thread = int2(groupThread);
    int2 pixel = origin + thread;

    // Stage the 16x16 region from four texels behind the tile to four past it.
    for (int k = 0; k < 4; ++k)
    {
        int2 offset = int2(k & 1, k >> 1) * 8;
        StageNeighbour(origin - 4 + thread + offset, thread + offset);
    }
    GroupMemoryBarrierWithGroupSync();

    if (!InsideExtent(pixel, g_PC.Extent))
        return;

    int2 local = thread + 4;
    float4 signal = g_Radiance[local.y][local.x];
    float variance = g_Variance[pixel];
    if (g_Glossy[local.y][local.x] > 0.0)
    {
        float samples = g_Textures[g_PC.SampleCountTexture].Load(int3(pixel, 0)).r;
        float4 average = g_Textures[g_PC.AverageTexture].SampleLevel(
            g_Samplers[g_PC.Sampler], TileAverageUV(pixel, g_PC.Extent), 0.0);
        float4 history = g_Textures[g_PC.ReprojectedTexture].Load(int3(pixel, 0));

        Moments neighbourhood = EstimateNeighbourhood(local);
        float4 spread = (sqrt(neighbourhood.Variance) + length(neighbourhood.Mean - average)) *
                        TemporalStabilityFactor * HistoryClipScale;
        float4 centre = lerp(neighbourhood.Mean, average, ClipCentreAverageShare);
        float4 clippedHistory = ClipAabb(centre - spread, centre + spread, history);
        float historyWeight = 1.0 - 1.0 / max(samples, 1.0);

        // A young history leans on the tile average, and an outlier is held to the tile's spread.
        signal = lerp(signal, average, 1.0 / max(samples + 1.0, 1.0));
        signal = ClipAabb(average - spread, average + spread, signal);

        signal = lerp(signal, clippedHistory, historyWeight);
        variance = lerp(TemporalVariance(signal.rgb, clippedHistory.rgb), variance, historyWeight);
        if (any(isinf(signal)) || any(isnan(signal)) || isinf(variance) || isnan(variance))
        {
            signal = float4(0.0, 0.0, 0.0, 0.0);
            variance = 0.0;
        }
    }
    g_Output[pixel] = signal;
    g_Variance[pixel] = variance;
}
