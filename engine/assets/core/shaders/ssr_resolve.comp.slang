// Stochastic SSR denoiser, third pass: blends each glossy pixel's filtered reflection into its
// reprojected history and writes the denoised reflection. One 8x8 workgroup per listed tile,
// dispatched indirectly by the tile count.
//
// - The history is clipped to a box built from the filtered signal's 3x3 neighbourhood: its mean plus
//   or minus HistoryClipGamma standard deviations, within the neighbourhood's minimum and maximum,
//   and widened to hold this pixel's own filtered value. Only glossy neighbours count. The colour is
//   clipped toward the box's centre in YCoCg (Playdead's ClipAABB) and the confidence clamped to its
//   own range. A box that tight
//   holds no blur or stale content a history could carry, and one always holding the pixel's own
//   value keeps a still, sparse reflection (a small bright source) at its level.
// - The blend takes 1 / samples of this frame, the sample count the reprojection set.
// - A glossy pixel whose filtered radiance is zero still resolves against its history: a miss is a
//   sample of the premultiplied signal, so a reflection hit on few frames accumulates rather than
//   starting again at every miss. A pixel that is not glossy passes through.
// - The variance is resolved in place: the temporal luminance variance between the result and the
//   clipped history, blended into the filter's by the history's weight.
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

// Set 0 is reserved for the bindless registry; the tile list and the pass's targets bind on set 3.
[[vk::binding(0, 3)]] StructuredBuffer<uint> g_TileList;
[[vk::binding(1, 3)]] [format("rgba16f")] RWTexture2D<float4> g_Output;
[[vk::binding(2, 3)]] [format("r16f")] RWTexture2D<float> g_Variance;

struct PushConstants
{
    uint PrefilteredTexture;   // the spatial filter's radiance
    uint ReprojectedTexture;   // the reprojection's history radiance
    uint SampleCountTexture;   // the reprojection's sample counts
    uint AlphaTexture;         // the classifier's GGX alpha
    uint2 Extent;              // render extent in pixels
    float MaxAlpha;            // glossy below this alpha
    uint Pad0;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

// The 10x10 neighbourhood of the filtered radiance around the tile, and whether each texel is glossy.
groupshared float4 g_Radiance[10][10];
groupshared float g_Glossy[10][10];

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

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint2 groupThread : SV_GroupThreadID, uint3 group : SV_GroupID)
{
    int2 origin = DenoiseTileOrigin(g_TileList, group.x);
    int2 thread = int2(groupThread);
    int2 pixel = origin + thread;

    // Stage the 10x10 region from one texel behind the tile to one past it.
    for (int index = thread.y * 8 + thread.x; index < 100; index += 64)
    {
        int2 local = int2(index % 10, index / 10);
        StageNeighbour(origin - 1 + local, local);
    }
    GroupMemoryBarrierWithGroupSync();

    if (!InsideExtent(pixel, g_PC.Extent))
        return;

    int2 local = thread + 1;
    float4 signal = g_Radiance[local.y][local.x];
    float variance = g_Variance[pixel];
    if (g_Glossy[local.y][local.x] > 0.0)
    {
        float samples = g_Textures[g_PC.SampleCountTexture].Load(int3(pixel, 0)).r;
        float4 history = g_Textures[g_PC.ReprojectedTexture].Load(int3(pixel, 0));

        float4 sum = float4(0.0, 0.0, 0.0, 0.0);
        float4 sumSquares = float4(0.0, 0.0, 0.0, 0.0);
        float4 lowest = float4(1e30, 1e30, 1e30, 1e30);
        float4 highest = float4(-1e30, -1e30, -1e30, -1e30);
        float count = 0.0;
        for (int y = -1; y <= 1; ++y)
        {
            for (int x = -1; x <= 1; ++x)
            {
                int2 neighbour = local + int2(x, y);
                if (g_Glossy[neighbour.y][neighbour.x] > 0.0)
                {
                    float4 radiance = g_Radiance[neighbour.y][neighbour.x];
                    float4 value = float4(RgbToYCoCg(radiance.rgb), radiance.a);
                    sum += value;
                    sumSquares += value * value;
                    lowest = min(lowest, value);
                    highest = max(highest, value);
                    count += 1.0;
                }
            }
        }
        float4 mean = sum / count;
        float4 deviation = sqrt(max(sumSquares / count - mean * mean, 0.0));
        float4 own = float4(RgbToYCoCg(signal.rgb), signal.a);
        float4 boxMin = min(max(mean - HistoryClipGamma * deviation, lowest), own);
        float4 boxMax = max(min(mean + HistoryClipGamma * deviation, highest), own);
        // The confidence is clamped on its own axis: clipped with the colour toward the box's
        // centre, a confidence just outside a narrow range would drag the colour toward a bright
        // neighbour's across a silhouette.
        float4 clippedHistory =
            float4(YCoCgToRgb(ClipAabb(boxMin.xyz, boxMax.xyz, RgbToYCoCg(history.rgb))),
                   clamp(history.a, boxMin.w, boxMax.w));
        float historyWeight = 1.0 - 1.0 / max(samples, 1.0);

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
