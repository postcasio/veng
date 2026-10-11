// Temporal anti-aliasing resolve: blends this frame's jittered HDR color with the
// reprojected history of previous frames so the jitter sequence integrates into a
// supersampled, edge-stable image. Runs in linear HDR before bloom and tonemap.
//
// The current frame is rasterized through a jittered projection into a sub-rect of the render
// allocation (the render scale — static, dynamic-resolution, or a TAAU upscale factor), while the
// history and this pass's output are unjittered and cover the whole output allocation. So each
// output pixel reconstructs the current frame at its own unjittered position: it finds the render
// texels around that position, offset by the jitter the view block publishes (JitterNdc), and
// weights each by its sample's distance from the pixel centre, measured in output pixels — a
// Gaussian fit of the Blackman-Harris window. At render scale 1 that is a one-pixel-wide filter
// centred where the pixel's own sample would have been without the jitter; under upscaling the
// window narrows to the output pixel, so a texel contributes to the output pixels its sample landed
// near, and the summed weight scales how much of this frame the pixel takes (a pixel no sample
// landed near this frame leans on its history). The neighborhood clip box is built from the same
// texels.
//
// Motion is per-object: geometry reads the screen-space motion vector the surface pass wrote into
// the g-buffer velocity channel (camera and object movement combined, measured between unjittered
// clip positions), at the nearest-depth texel of the 3x3 so a silhouette pixel follows the
// foreground. The background, which the surface pass leaves at the cleared depth, reprojects as a
// direction through PrevFromCurDirection, unjittered on both sides, so a camera that has not turned
// fetches its history from exactly the pixel it writes.
//
// History is rejected where it cannot be trusted — offscreen reprojection or an
// invalid history frame (the first frame, or the frame after a resize) fall back to
// the current color. Where it is trusted, the history is constrained to the local
// neighborhood with YCoCg variance clipping (suppressing ghosting at disocclusions
// and moving silhouettes) and sampled with a Catmull-Rom filter (countering the
// blur a bilinear history accumulates). The blend is luminance-weighted on the exposed
// luminance to damp fireflies (the Karis weighting), the stability mechanism the bloom
// bright-pass's per-tap ceiling complements.
#include "view_constants.slang"

[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];

struct PushConstants
{
    uint CurrentTexture;  // this frame's jittered HDR color (bindless index)
    uint HistoryTexture;  // previous resolved HDR (bindless index)
    uint DepthTexture;    // depth for the nearest-depth search (bindless index)
    uint VelocityTexture; // per-object motion vectors for geometry (bindless index)
    uint Sampler;         // shared linear clamp sampler (the history's Catmull-Rom taps)
    uint ViewConstantsIndex; // current frame-in-flight's view-constants region
    uint HistoryValid;    // 0 on the first frame / after a resize → ignore history
    float Exposure;       // the exposure the tonemap applies this frame (the Karis weights' scale)
    uint2 Extent;         // full output/history pixel dimensions (Catmull-Rom + output grid)
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

// History blend weight: the fraction of the current frame mixed in each frame. A
// smaller value accumulates more history (smoother, slower to converge); 0.1 keeps
// ~10 effective frames, a standard responsiveness/stability balance.
static const float BlendAlpha = 0.1;

// Variance-clip width in standard deviations. A wider box keeps more history
// (less ghosting rejection); 1.25 is the common robust setting.
static const float ClipGamma = 1.25;

// The reconstruction weight's falloff, exp(-KernelFalloff * d^2) with d in output pixels: the
// Gaussian fit of a Blackman-Harris window one pixel wide.
static const float KernelFalloff = 2.29;

// How far the reconstruction window may narrow under upscaling, as a ratio of output to render
// pixels. Past it the jitter period's samples no longer land within the window of every output
// pixel, so the window stops at this width rather than leave pixels the current frame never reaches.
static const float MaxKernelSharpening = 2.5;

float3 RgbToYCoCg(float3 c)
{
    return float3(0.25 * (c.r + 2.0 * c.g + c.b), 0.5 * (c.r - c.b),
                  0.25 * (-c.r + 2.0 * c.g - c.b));
}

float3 YCoCgToRgb(float3 c)
{
    const float t = c.x - c.z;
    return float3(t + c.y, c.x + c.z, t - c.y);
}

float Luminance(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// Clip the history color toward the current neighborhood's AABB along the segment
// from the box center, rather than clamping per channel: clipping preserves hue and
// reintroduces less aliasing than a component-wise clamp.
float3 ClipToAabb(float3 boxMin, float3 boxMax, float3 history)
{
    const float3 center = 0.5 * (boxMax + boxMin);
    const float3 extent = 0.5 * (boxMax - boxMin) + 1e-5;
    const float3 offset = history - center;
    const float3 unit = offset / extent;
    const float3 absUnit = abs(unit);
    const float maxUnit = max(absUnit.x, max(absUnit.y, absUnit.z));
    if (maxUnit > 1.0)
    {
        return center + offset / maxUnit;
    }
    return history;
}

// Catmull-Rom sampling of a bilinearly-filtered texture: reconstructs a sharp history
// sample from the 4×4 texels around uv, collapsing the sixteen taps into nine bilinear
// fetches. Counters the per-frame blur a bilinear history accumulates.
float3 SampleHistoryCatmullRom(uint texture, uint samp, float2 uv, float2 texelSize)
{
    const float2 resolution = float2(g_PC.Extent);
    const float2 samplePos = uv * resolution;
    const float2 texPos1 = floor(samplePos - 0.5) + 0.5;

    const float2 f = samplePos - texPos1;
    const float2 w0 = f * (-0.5 + f * (1.0 - 0.5 * f));
    const float2 w1 = 1.0 + f * f * (-2.5 + 1.5 * f);
    const float2 w2 = f * (0.5 + f * (2.0 - 1.5 * f));
    const float2 w3 = f * f * (-0.5 + 0.5 * f);

    const float2 w12 = w1 + w2;
    const float2 offset12 = w2 / w12;

    const float2 texPos0 = (texPos1 - 1.0) * texelSize;
    const float2 texPos3 = (texPos1 + 2.0) * texelSize;
    const float2 texPos12 = (texPos1 + offset12) * texelSize;

    SamplerState s = g_Samplers[NonUniformResourceIndex(samp)];
    Texture2D t = g_Textures[NonUniformResourceIndex(texture)];

    float3 result = 0.0;
    result += t.Sample(s, float2(texPos0.x, texPos0.y)).rgb * w0.x * w0.y;
    result += t.Sample(s, float2(texPos12.x, texPos0.y)).rgb * w12.x * w0.y;
    result += t.Sample(s, float2(texPos3.x, texPos0.y)).rgb * w3.x * w0.y;

    result += t.Sample(s, float2(texPos0.x, texPos12.y)).rgb * w0.x * w12.y;
    result += t.Sample(s, float2(texPos12.x, texPos12.y)).rgb * w12.x * w12.y;
    result += t.Sample(s, float2(texPos3.x, texPos12.y)).rgb * w3.x * w12.y;

    result += t.Sample(s, float2(texPos0.x, texPos3.y)).rgb * w0.x * w3.y;
    result += t.Sample(s, float2(texPos12.x, texPos3.y)).rgb * w12.x * w3.y;
    result += t.Sample(s, float2(texPos3.x, texPos3.y)).rgb * w3.x * w3.y;

    return max(result, 0.0);
}

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target0
{
    // uv is the output pixel's centre: unjittered, in the full output/history allocation.
    const float2 uv = input.v_UV;
    ViewConstants vc = LoadViewConstants(g_PC.ViewConstantsIndex);
    // The current frame's rendered sub-rect extent; Extent is the full output/history extent.
    const float2 renderExtent = vc.ExtentParams.xy;
    const float2 outputTexel = 1.0 / float2(g_PC.Extent);
    const float2 kernelScale =
        clamp(float2(g_PC.Extent) / renderExtent, 1.0, MaxKernelSharpening);
    Texture2D current = g_Textures[NonUniformResourceIndex(g_PC.CurrentTexture)];
    Texture2D depthTexture = g_Textures[NonUniformResourceIndex(g_PC.DepthTexture)];

    // Where this pixel's unjittered position fell in the jittered render: the scene moved by
    // JitterNdc, which is JitterNdc / 2 of the extent in UV.
    const float2 samplePos = (uv + 0.5 * vc.JitterNdc.xy) * renderExtent;
    const int2 centerTexel = int2(floor(samplePos));
    const int2 maxTexel = int2(renderExtent) - 1;

    // One pass over the 3×3 render texels around it: the weighted reconstruction, the YCoCg
    // neighborhood statistics (mean and variance bound the history, min/max widen the box for thin
    // features), and the nearest depth (reverse-Z: the largest) with the texel holding it.
    float3 colorSum = 0.0;
    float weightSum = 0.0;
    float3 m1 = 0.0;
    float3 m2 = 0.0;
    float3 neighborMin = 1e9;
    float3 neighborMax = -1e9;
    float nearestDepth = 0.0;
    int2 nearestTexel = clamp(centerTexel, int2(0), maxTexel);
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const int2 texel = clamp(centerTexel + int2(x, y), int2(0), maxTexel);
            const float3 tap = current.Load(int3(texel, 0)).rgb;
            const float2 offset = (float2(texel) + 0.5 - samplePos) * kernelScale;
            const float weight = exp(-KernelFalloff * dot(offset, offset));
            colorSum += tap * weight;
            weightSum += weight;

            const float3 ycc = RgbToYCoCg(tap);
            m1 += ycc;
            m2 += ycc * ycc;
            neighborMin = min(neighborMin, ycc);
            neighborMax = max(neighborMax, ycc);

            const float depth = depthTexture.Load(int3(texel, 0)).r;
            if (depth > nearestDepth)
            {
                nearestDepth = depth;
                nearestTexel = texel;
            }
        }
    }

    const float3 color = colorSum / max(weightSum, 1e-6);
    const float3 mean = m1 / 9.0;
    const float3 sigma = sqrt(max(m2 / 9.0 - mean * mean, 0.0));
    const float3 boxMin = max(mean - ClipGamma * sigma, neighborMin);
    const float3 boxMax = min(mean + ClipGamma * sigma, neighborMax);

    // Find this pixel's previous-frame screen position. Geometry anywhere in the 3×3 reprojects by
    // the nearest one's motion vector (full-frame UV, so prevUv is full-resolution); with only the
    // cleared depth around, the pixel reprojects as a direction.
    float2 prevUv;
    bool reprojected = true;
    if (nearestDepth > 0.0)
    {
        const float2 velocity =
            g_Textures[NonUniformResourceIndex(g_PC.VelocityTexture)].Load(int3(nearestTexel, 0)).rg;
        prevUv = uv - velocity;
    }
    else
    {
        const float4 prevClip = mul(vc.PrevFromCurDirection, float4(uv * 2.0 - 1.0, 0.0, 1.0));
        reprojected = prevClip.w > 0.0;
        prevUv = (prevClip.xy / prevClip.w) * 0.5 + 0.5;
    }

    // History is untrustworthy when it does not exist yet or reprojects offscreen.
    const bool offscreen = !reprojected || any(prevUv < 0.0) || any(prevUv > 1.0);
    if (g_PC.HistoryValid == 0 || offscreen)
    {
        return float4(color, 1.0);
    }

    float3 history =
        SampleHistoryCatmullRom(g_PC.HistoryTexture, g_PC.Sampler, prevUv, outputTexel);
    history = YCoCgToRgb(ClipToAabb(boxMin, boxMax, RgbToYCoCg(history)));

    // This frame's share: the full blend where a sample landed on the pixel, less where the nearest
    // one fell between output pixels (only under upscaling; at scale 1 the sum never drops below 1).
    const float alpha = BlendAlpha * saturate(weightSum);

    // Luminance-weighted blend on the luminance the tonemap will see: a brighter sample carries less
    // weight, so a transient firefly cannot dominate the accumulated history, at every exposure.
    const float currentWeight = alpha / (1.0 + Luminance(color) * g_PC.Exposure);
    const float historyWeight = (1.0 - alpha) / (1.0 + Luminance(history) * g_PC.Exposure);
    const float3 resolved =
        (color * currentWeight + history * historyWeight) / (currentWeight + historyWeight);

    return float4(resolved, 1.0);
}
