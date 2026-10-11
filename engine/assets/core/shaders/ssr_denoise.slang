// Shared by the stochastic reflection denoiser's passes (ssr_reproject.comp, ssr_prefilter.comp and
// ssr_resolve.comp): its constants, the tile each workgroup covers, and the luminance, variance,
// depth, colour-space and clip helpers.
//
// Each pass runs one 8x8 workgroup per listed tile; the filter and the resolve stage the
// neighbourhood around it in workgroup memory. A texel outside the render extent stages as zero.
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
// The disocclusion factor exp(-|1 - n·n'| NormalWeight) exp(-|Δz| / z DepthWeight), and the factor
// below which a history is searched for, then dropped.
static const float DisocclusionNormalWeight = 1.4;
static const float DisocclusionDepthWeight = 1.0;
static const float DisocclusionThreshold = 0.9;
// The virtual-point candidate is taken only where its history normal agrees this closely.
static const float ReprojectionNormalSimilarityThreshold = 0.9999;
// The longest history a still pixel keeps, scaled by SamplesForRoughness, and the shortest cap on
// it.
static const float MaxHistorySamples = 32.0;
static const float MinHistorySamples = 8.0;
// The history a moving reflection keeps: the still cap divided by
// 1 + motion / HistoryMotionPixels + misplacement / HistoryMisplacementPixels, never below
// MovingHistorySamples. The motion is how far the reflected image moved this frame, which a
// Catmull-Rom read resamples nearly losslessly; the misplacement is how far from the reflection the
// history was read, which carries other content.
static const float HistoryMotionPixels = 2.0;
static const float HistoryMisplacementPixels = 0.25;
static const float MovingHistorySamples = 2.0;
// A hit has jumped when last frame's ray lengths around the history and this frame's around the
// pixel do not overlap within this factor.
static const float RayLengthJump = 2.0;
// The temporal resolve's clip box: the 3x3 neighbourhood's YCoCg mean plus or minus this many
// standard deviations, within the neighbourhood's range.
static const float HistoryClipGamma = 1.25;
// The view depth taken for a cleared or far-plane depth, so a disocclusion against it is total.
static const float FarDepth = 1e30;

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

// The history clip's colour space: its luminance and chroma axes keep a hue when a box is narrow.
float3 RgbToYCoCg(float3 c)
{
    return float3(0.25 * (c.r + 2.0 * c.g + c.b), 0.5 * (c.r - c.b),
                  0.25 * (-c.r + 2.0 * c.g - c.b));
}

float3 YCoCgToRgb(float3 c)
{
    float t = c.x - c.z;
    return float3(t + c.y, c.x + c.z, t - c.y);
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
float3 ClipAabb(float3 aabbMin, float3 aabbMax, float3 history)
{
    float3 centre = 0.5 * (aabbMax + aabbMin);
    float3 extentClip = 0.5 * (aabbMax - aabbMin) + 0.001;
    float3 offset = history - centre;
    float3 offsetClip = abs(offset / extentClip);
    float maxUnit = max(offsetClip.x, max(offsetClip.y, offsetClip.z));
    return maxUnit > 1.0 ? centre + offset / maxUnit : history;
}
