// Temporal anti-aliasing resolve: blends this frame's jittered HDR color with the
// reprojected history of previous frames so the jitter sequence integrates into a
// supersampled, edge-stable image. Runs in linear HDR before bloom and tonemap.
//
// Motion is per-object: geometry reads the screen-space motion vector the surface pass
// wrote into the g-buffer velocity channel (which combines camera and object movement),
// so both a moving camera and a moving object reproject correctly. The background, which
// the surface pass leaves cleared, reprojects by reconstructing its far-plane world
// position from depth and projecting through the previous frame's unjittered view-projection.
//
// History is rejected where it cannot be trusted — offscreen reprojection or an
// invalid history frame (the first frame, or the frame after a resize) fall back to
// the current color. Where it is trusted, the history is constrained to the local
// neighborhood with YCoCg variance clipping (suppressing ghosting at disocclusions
// and moving silhouettes) and sampled with a Catmull-Rom filter (countering the
// blur a bilinear history accumulates). The blend is luminance-weighted to damp
// fireflies (the Karis weighting), the stability mechanism the bloom bright-pass's
// Karis average complements.
#include "view_constants.slang"

[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];

struct PushConstants
{
    uint CurrentTexture;  // this frame's jittered HDR color (bindless index)
    uint HistoryTexture;  // previous resolved HDR (bindless index)
    uint DepthTexture;    // depth for background reprojection (bindless index)
    uint VelocityTexture; // per-object motion vectors for geometry (bindless index)
    uint Sampler;         // shared linear clamp sampler
    uint ViewConstantsIndex; // current frame-in-flight's view-constants region
    uint HistoryValid;    // 0 on the first frame / after a resize → ignore history
    uint Pad0;            // pads to an even uint count so uint2 Extent is 8-byte aligned
    uint2 Extent;         // target pixel dimensions (for texel size and 3×3 taps)
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

// Five-tap Catmull-Rom sampling of a bilinearly-filtered texture: reconstructs a
// sharp history sample from the 3×3 footprint around uv, collapsing the nine taps
// into five bilinear fetches. Counters the per-frame blur a bilinear history
// accumulates.
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
    const float2 uv = input.v_UV;
    const float2 texelSize = 1.0 / float2(g_PC.Extent);
    SamplerState samp = g_Samplers[NonUniformResourceIndex(g_PC.Sampler)];
    Texture2D current = g_Textures[NonUniformResourceIndex(g_PC.CurrentTexture)];

    const float3 color = current.Sample(samp, uv).rgb;

    // The 3×3 current neighborhood in YCoCg: its mean and variance bound the
    // history (variance clipping), and its min/max widen the box for thin features.
    float3 m1 = 0.0;
    float3 m2 = 0.0;
    float3 neighborMin = 1e9;
    float3 neighborMax = -1e9;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const float3 tap = current.Sample(samp, uv + float2(x, y) * texelSize).rgb;
            const float3 ycc = RgbToYCoCg(tap);
            m1 += ycc;
            m2 += ycc * ycc;
            neighborMin = min(neighborMin, ycc);
            neighborMax = max(neighborMax, ycc);
        }
    }

    const float3 mean = m1 / 9.0;
    const float3 sigma = sqrt(max(m2 / 9.0 - mean * mean, 0.0));
    const float3 boxMin = max(mean - ClipGamma * sigma, neighborMin);
    const float3 boxMax = min(mean + ClipGamma * sigma, neighborMax);

    // Find this pixel's previous-frame screen position. Geometry uses the per-object
    // motion vector the surface pass wrote into the g-buffer (camera + object motion); the
    // background (cleared depth) has no velocity, so it reprojects by reconstructing the
    // far-plane world position from depth and projecting through last frame's view-projection.
    const float depth = g_Textures[NonUniformResourceIndex(g_PC.DepthTexture)].Sample(samp, uv).r;
    ViewConstants vc = LoadViewConstants(g_PC.ViewConstantsIndex);

    float2 prevUv;
    if (depth < 1.0)
    {
        const float2 velocity =
            g_Textures[NonUniformResourceIndex(g_PC.VelocityTexture)].Sample(samp, uv).rg;
        prevUv = uv - velocity;
    }
    else
    {
        const float2 ndc = uv * 2.0 - 1.0;
        const float4 worldH = mul(vc.InvViewProj, float4(ndc, depth, 1.0));
        const float3 world = worldH.xyz / worldH.w;
        const float4 prevClip = mul(vc.PrevViewProj, float4(world, 1.0));
        prevUv = (prevClip.xy / prevClip.w) * 0.5 + 0.5;
    }

    // History is untrustworthy when it does not exist yet or reprojects offscreen.
    const bool offscreen = any(prevUv < 0.0) || any(prevUv > 1.0);
    if (g_PC.HistoryValid == 0 || offscreen)
    {
        return float4(color, 1.0);
    }

    float3 history = SampleHistoryCatmullRom(g_PC.HistoryTexture, g_PC.Sampler, prevUv, texelSize);
    history = YCoCgToRgb(ClipToAabb(boxMin, boxMax, RgbToYCoCg(history)));

    // Luminance-weighted blend: a brighter sample carries less weight, so a transient
    // firefly cannot dominate the accumulated history.
    const float currentWeight = BlendAlpha / (1.0 + Luminance(color));
    const float historyWeight = (1.0 - BlendAlpha) / (1.0 + Luminance(history));
    const float3 resolved =
        (color * currentWeight + history * historyWeight) / (currentWeight + historyWeight);

    return float4(resolved, 1.0);
}
