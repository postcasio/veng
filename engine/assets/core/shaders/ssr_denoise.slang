// Shared by the stochastic reflection denoiser's passes (ssr_reproject.comp, ssr_prefilter.comp and
// ssr_resolve.comp): its constants, the tile each workgroup covers, and the luminance, variance,
// depth and clip helpers.
//
// Each pass runs one 8x8 workgroup per listed tile and stages the 16x16 neighbourhood around it, four
// texels a thread, in workgroup memory. A texel outside the render extent stages as zero.
//
// The signal is the intersection's radiance premultiplied by its confidence, (rgb·c, c). The fourth
// channel rides through every blend, weight and clip as the other three do; the luminance
// statistics and the radiance distances are taken on rgb·c. A pixel that is not glossy reflects
// nothing (its zero is not a sample of a reflection), so it carries no weight in a neighbourhood
// statistic, the spatial filter or the tile average.
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
#include "ssr_surface.slang"
#include "Veng/ssr_ray_list.slang"
#include "Veng/ssr_sample.slang"

// The 9x9 Gaussian over which the reprojection and the temporal resolve take the neighbourhood's
// moments: exp(-K i^2 / (Radius + 1)^2) per axis.
static const int NeighbourhoodRadius = 4;
static const float NeighbourhoodGaussianK = 3.0;

// The spatial filter's radiance weight, exp(-(Bias + variance * VarianceK) * distance). The distance
// is absolute radiance, so any bias holds a bright reflection below its tile average however
// settled it is, and a small bright source loses most of its energy. With no bias only a young or
// unsettled history, whose variance is high, damps an outlier.
static const float RadianceWeightBias = 0.0;
static const float RadianceWeightVarianceK = 0.1;
// The tile average weighs each pixel by exp(-luminance * this), so a firefly moves it little.
static const float AverageRadianceLuminanceWeight = 0.3;
// The spatial filter's variance weight, max(Bias, 1 - exp(-variance * Weight)).
static const float PrefilterVarianceWeight = 4.4;
static const float PrefilterVarianceBias = 0.1;
// The spatial filter's edge stops: pow(n·n', NormalSigma) and exp(-|Δz| z DepthSigma).
static const float PrefilterNormalSigma = 512.0;
static const float PrefilterDepthSigma = 4.0;
// The surface candidate is kept while its squared distance from the neighbourhood mean is under
// this multiple of the neighbourhood's variance.
static const float SurfaceDiscardVarianceWeight = 1.5;
// The disocclusion factor exp(-|1 - n·n'| NormalWeight) exp(-|Δz| / z DepthWeight), and the factor
// below which a history is searched for, then dropped.
static const float DisocclusionNormalWeight = 1.4;
static const float DisocclusionDepthWeight = 1.0;
static const float DisocclusionThreshold = 0.9;
// The virtual-point candidate is taken only where its history normal agrees this closely.
static const float ReprojectionNormalSimilarityThreshold = 0.9999;
// The longest history a pixel keeps, scaled by SamplesForRoughness, and the shortest cap on it.
static const float MaxHistorySamples = 32.0;
static const float MinHistorySamples = 8.0;
// The temporal resolve's clip box is the neighbourhood's spread times this, times the clip scale.
static const float HistoryClipScale = 1.4;
// How far toward the tile average the clip box's centre moves.
static const float ClipCentreAverageShare = 0.2;
// The view depth taken for a cleared or far-plane depth, so a disocclusion against it is total.
static const float FarDepth = 1e30;

// The reference's temporal stability factor, the share of the neighbourhood's spread the history
// clip keeps.
static const float TemporalStabilityFactor = 0.7;

// The share of the maximum history length a lobe of GGX width `alpha` keeps.
float SamplesForRoughness(float alpha)
{
    return 1.0 - exp(-alpha * 100.0);
}

bool IsGlossy(float alpha, float maxAlpha)
{
    return alpha < maxAlpha;
}

bool IsMirror(float alpha)
{
    return alpha < SsrMirrorAlpha;
}

float DenoiseLuminance(float3 colour)
{
    return max(dot(colour, float3(0.299, 0.587, 0.114)), 0.001);
}

// The squared relative luminance change between a history and this frame's radiance.
float TemporalVariance(float3 history, float3 radiance)
{
    float historyLuminance = DenoiseLuminance(history);
    float luminance = DenoiseLuminance(radiance);
    float difference =
        abs(historyLuminance - luminance) / max(max(historyLuminance, luminance), 0.5);
    return difference * difference;
}

float NeighbourhoodKernelWeight(float offset)
{
    float radius = float(NeighbourhoodRadius) + 1.0;
    return exp(-NeighbourhoodGaussianK * (offset * offset) / (radius * radius));
}

// A unit normal, or zero for a texel with none (the cleared background, a staged texel off screen).
float3 SafeNormalize(float3 normal)
{
    float length2 = dot(normal, normal);
    return length2 > 1e-12 ? normal * rsqrt(length2) : float3(0.0, 0.0, 0.0);
}

// The view depth (distance in front of the camera) at screen UV `uv` and reverse-Z device depth
// `depth`, through this frame's projection.
float LinearDepth(ViewConstants vc, float2 uv, float depth)
{
    if (depth <= 0.0)
        return FarDepth;
    return -mul((float3x3)vc.View, RelativePosition(vc, uv, depth)).z;
}

// The origin pixel of the tile workgroup `group` covers.
int2 DenoiseTileOrigin(StructuredBuffer<uint> tiles, uint group)
{
    return int2(SsrUnpackTile(tiles[group]));
}

// Whether `pixel` lies inside the render extent.
bool InsideExtent(int2 pixel, uint2 extent)
{
    return all(pixel >= int2(0, 0)) && all(pixel < int2(extent));
}

// UV of a pixel against the tile-average target's extent, which covers whole tiles.
float2 TileAverageUV(int2 pixel, uint2 extent)
{
    uint2 rounded = (extent + 7u) & ~7u;
    return (float2(pixel) + 0.5) / float2(rounded);
}

// Clips `history` toward the box's centre onto the box, where it lies outside.
//
// From "Temporal Reprojection Anti-Aliasing", https://github.com/playdeadgames/temporal, under this
// licence:
//
// Copyright (c) [2015] [Playdead]
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
float4 ClipAabb(float4 aabbMin, float4 aabbMax, float4 history)
{
    float4 centre = 0.5 * (aabbMax + aabbMin);
    float4 extentClip = 0.5 * (aabbMax - aabbMin) + 0.001;
    float4 offset = history - centre;
    float4 offsetClip = abs(offset / extentClip);
    float maxUnit = max(max(offsetClip.x, offsetClip.y), max(offsetClip.z, offsetClip.w));
    return maxUnit > 1.0 ? centre + offset / maxUnit : history;
}

// The neighbourhood's weighted mean and variance, per channel.
struct Moments
{
    float4 Mean;
    float4 Variance;
};
