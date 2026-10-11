// SSR temporal resolve: under a temporal resolve, accumulates the reflection term the composite
// wrote (ssr_composite fsTerm) into a history of its own and adds the result to the resolved scene.
//
// The frame's resolve reprojects each pixel by its surface's motion, but a reflected image moves
// with the reflected geometry's depth, so a reflection inside the frame's history lags it under any
// camera translation. A glossy normal-mapped surface also turns every reflected ray at each jitter
// phase, since each phase samples the normal map somewhere else, and an exponential history taking
// a tenth of each frame leaves that period-long swing a visible pulse. So the reflection is resolved
// here instead, and kept out of the frame's history:
//
//  - Reconstruction. Each output pixel rebuilds this frame's term at its own unjittered position
//    from the 3x3 render texels around where that position fell in the jittered render, with the
//    frame resolve's own Gaussian fit of a Blackman-Harris window, narrowed the same way under
//    upscaling. The history is unjittered and covers the resolve's output allocation, so a still
//    camera reads it back from exactly the pixel it writes, and nothing resamples it.
//  - Reprojection. The history is read where the reflected image stood last frame: the composite
//    projects the reflected hit, unfolded through the surface along the pixel's ray, through both
//    frames' unjittered matrices. A plane mirror's image of a point is exactly there. The motion is
//    the one of the 3x3 render texels whose reflected image is nearest, as the frame resolve takes
//    the nearest surface's: a thin reflected feature spreads into texels whose own rays reflect what
//    lies behind it, which moves slower under a camera translation, and its history would trail.
//  - Accumulation. The history carries a sample count, and each frame takes one share in that
//    count, so a pixel averages every jitter phase alike rather than weighting the latest. The count
//    runs to StillSamples, and while the reflected image moves, to as many samples as span
//    MovingReach pixels of its travel, never fewer than MovingSamples: two jitter periods, so a
//    moving history still weights every phase alike. What a reflection on a curved or normal-mapped
//    surface shows changes with the viewpoint, and each reprojected read filters the history a
//    little, so how long a history stays right depends on how far its image has travelled.
//  - Validation. A box built from this frame's 3x3 texels cannot validate a still history: under a
//    normal-mapped surface those texels swing together from phase to phase, and a history clipped
//    to them follows the swing. So beside the history runs a fast one, the luminance over about one
//    jitter period, which is clipped to that box every frame: it ripples about the history's mean
//    through a period, and leaves it within a frame or two when the neighbourhood really changes.
//    The history learns how far the fast one strays from it, and loses its count when the fast one
//    strays several times further — a reflected source that appeared or went, a hit that changed, a
//    surface newly revealed. A moving history is validated the same way and never clipped to this
//    frame's box, which would follow the swing. The first period of a history is not validated,
//    since its spread is not learnt yet.
//    A history read off-screen, a pixel that reflected nothing, and the first frame after a recreate
//    keep no history at all.
#include "view_constants.slang"

[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];

struct PushConstants
{
    uint TermTexture;        // this frame's reflection term (jittered, render allocation); alpha 1 where evaluated
    uint MotionTexture;      // the reflected image's unjittered motion and view depth (render allocation)
    uint HistoryTexture;     // the previous accumulation (output allocation); count in alpha
    uint FastTexture;        // the previous fast luminance, and its squared spread about the history
    uint Sampler;            // shared linear clamp sampler (the history's Catmull-Rom taps)
    uint ViewConstantsIndex; // current frame-in-flight's view-constants region
    uint HistoryValid;       // 0 on the first frame after a recreate: ignore the history
    uint Pad0;
    uint2 Extent;            // output/history pixel dimensions
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

struct TemporalOutput
{
    float4 History : SV_Target0;  // the accumulated term, its sample count in alpha
    float2 Fast : SV_Target1;     // the fast luminance, and its squared spread about the history
    float4 Scene : SV_Target2;    // the accumulated term, added onto the resolved scene
};

// The frame resolve's reconstruction weight, exp(-KernelFalloff * d^2) with d in output pixels, and
// how far it may narrow under upscaling (taa_resolve.frag).
static const float KernelFalloff = 2.29;
static const float MaxKernelSharpening = 2.5;

// The sample count a pixel's history runs to: a period-long swing of the term at each jitter phase
// moves the average by about its size over this count each frame.
static const float StillSamples = 64.0;
// How far, in output pixels, the reflected image may travel across the samples a moving history
// holds, and the fewest samples it is held to: two jitter periods.
static const float MovingReach = 64.0;
static const float MovingSamples = 16.0;
// The fast luminance's sample count: one jitter period.
static const float FastSamples = 8.0;

// How far past the neighbourhood's own range the fast luminance's box reaches, in its standard
// deviations.
static const float ClipSpread = 1.0;
// How far the fast luminance may stray from the history before the history is doubted: this many
// times its learnt spread, plus a share of the larger of the two luminances.
static const float ChangeSpread = 2.5;
static const float ChangeFloor = 0.05;
// The share a history allows while it learns its spread, narrowing to ChangeFloor over
// TrustSamples samples: wide enough for a jitter swing, narrow enough for a source appearing.
static const float LearningFloor = 0.5;
static const float TrustSamples = 8.0;
// How far past the tolerance, in its own units, a history keeps none of its count.
static const float ClipReset = 1.0;

// The least reconstructed share of reflecting texels for the pixel to keep a history.
static const float ReflectsFloor = 1e-3;

float3 RgbToYCoCg(float3 c)
{
    return float3(0.25 * (c.r + 2.0 * c.g + c.b), 0.5 * (c.r - c.b), 0.25 * (-c.r + 2.0 * c.g - c.b));
}

// Five-tap Catmull-Rom read of the history at uv (as taa_resolve.frag reads its own), so a moving
// pixel's reprojected read does not blur the history as a bilinear one would.
float3 SampleHistoryCatmullRom(Texture2D t, SamplerState s, float2 uv, float2 texelSize)
{
    const float2 samplePos = uv * float2(g_PC.Extent);
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

    float3 result = 0.0;
    result += t.SampleLevel(s, float2(texPos0.x, texPos0.y), 0.0).rgb * w0.x * w0.y;
    result += t.SampleLevel(s, float2(texPos12.x, texPos0.y), 0.0).rgb * w12.x * w0.y;
    result += t.SampleLevel(s, float2(texPos3.x, texPos0.y), 0.0).rgb * w3.x * w0.y;
    result += t.SampleLevel(s, float2(texPos0.x, texPos12.y), 0.0).rgb * w0.x * w12.y;
    result += t.SampleLevel(s, float2(texPos12.x, texPos12.y), 0.0).rgb * w12.x * w12.y;
    result += t.SampleLevel(s, float2(texPos3.x, texPos12.y), 0.0).rgb * w3.x * w12.y;
    result += t.SampleLevel(s, float2(texPos0.x, texPos3.y), 0.0).rgb * w0.x * w3.y;
    result += t.SampleLevel(s, float2(texPos12.x, texPos3.y), 0.0).rgb * w12.x * w3.y;
    result += t.SampleLevel(s, float2(texPos3.x, texPos3.y), 0.0).rgb * w3.x * w3.y;
    return max(result, 0.0);
}

// The history read where the pixel's reflected image stood last frame.
struct Reprojected
{
    bool Valid;       // the read landed on screen and a history exists
    float3 Value;     // the accumulated term there
    float Count;      // its sample count
    float Fast;       // the fast luminance there
    float Spread2;    // the fast luminance's squared spread about the history there
};

Reprojected ReadHistory(float2 uv, float2 motion)
{
    Reprojected candidate;
    candidate.Value = float3(0.0, 0.0, 0.0);
    candidate.Count = 0.0;
    candidate.Fast = 0.0;
    candidate.Spread2 = 0.0;
    const float2 prevUv = uv - motion;
    candidate.Valid = g_PC.HistoryValid != 0u && all(prevUv >= 0.0) && all(prevUv <= 1.0);
    if (!candidate.Valid)
        return candidate;
    Texture2D history = g_Textures[NonUniformResourceIndex(g_PC.HistoryTexture)];
    SamplerState samp = g_Samplers[NonUniformResourceIndex(g_PC.Sampler)];
    candidate.Value = SampleHistoryCatmullRom(history, samp, prevUv, 1.0 / float2(g_PC.Extent));
    candidate.Count = history.SampleLevel(samp, prevUv, 0.0).a;
    const float2 fast =
        g_Textures[NonUniformResourceIndex(g_PC.FastTexture)].SampleLevel(samp, prevUv, 0.0).rg;
    candidate.Fast = fast.x;
    candidate.Spread2 = fast.y;
    candidate.Valid = candidate.Count > 0.0;
    return candidate;
}

[shader("fragment")]
TemporalOutput fsMain(VSOutput input)
{
    // uv is the output pixel's centre: unjittered, in the full output/history allocation.
    const float2 uv = input.v_UV;
    ViewConstants vc = LoadViewConstants(g_PC.ViewConstantsIndex);
    const float2 renderExtent = vc.ExtentParams.xy;
    const float2 kernelScale = clamp(float2(g_PC.Extent) / renderExtent, 1.0, MaxKernelSharpening);
    Texture2D term = g_Textures[NonUniformResourceIndex(g_PC.TermTexture)];

    // Where this pixel's unjittered position fell in the jittered render.
    const float2 samplePos = (uv + 0.5 * vc.JitterNdc.xy) * renderExtent;
    const int2 centerTexel = int2(floor(samplePos));
    const int2 maxTexel = int2(renderExtent) - 1;

    // The weighted reconstruction, the YCoCg range and moments of the 3x3, and the motion of the
    // texel whose reflected image is nearest, which the pixel reprojects by.
    float4 termSum = 0.0;
    float weightSum = 0.0;
    float3 m1 = 0.0;
    float3 m2 = 0.0;
    float3 neighborMin = 1e9;
    float3 neighborMax = -1e9;
    Texture2D motionTexture = g_Textures[NonUniformResourceIndex(g_PC.MotionTexture)];
    float2 motion = float2(0.0, 0.0);
    float nearestImage = 1e30;
    [unroll]
    for (int y = -1; y <= 1; ++y)
    {
        [unroll]
        for (int x = -1; x <= 1; ++x)
        {
            const int2 texel = clamp(centerTexel + int2(x, y), int2(0), maxTexel);
            const float4 tap = term.Load(int3(texel, 0));
            const float2 offset = (float2(texel) + 0.5 - samplePos) * kernelScale;
            const float weight = exp(-KernelFalloff * dot(offset, offset));
            termSum += tap * weight;
            weightSum += weight;
            // A texel that reflected nothing wrote no depth.
            const float3 imageMotion = motionTexture.Load(int3(texel, 0)).rgb;
            if (imageMotion.z > 0.0 && imageMotion.z < nearestImage)
            {
                nearestImage = imageMotion.z;
                motion = imageMotion.xy;
            }

            const float3 ycc = RgbToYCoCg(tap.rgb);
            m1 += ycc;
            m2 += ycc * ycc;
            neighborMin = min(neighborMin, ycc);
            neighborMax = max(neighborMax, ycc);
        }
    }

    TemporalOutput output;
    output.History = float4(0.0, 0.0, 0.0, 0.0);
    output.Fast = float2(0.0, 0.0);
    output.Scene = float4(0.0, 0.0, 0.0, 1.0);

    const float4 current = termSum / max(weightSum, 1e-6);
    // Nothing around this pixel reflects: there is nothing to add and no history to keep.
    if (current.a < ReflectsFloor)
        return output;
    const float currentLuma = RgbToYCoCg(current.rgb).x;

    // This frame's share: a full sample where one landed on the pixel, less under upscaling where
    // the nearest one fell between output pixels (at scale 1 the sum never drops below 1).
    const float share = saturate(weightSum);

    const Reprojected previous = ReadHistory(uv, motion);

    const float3 mean = m1 / 9.0;
    const float3 sigma = sqrt(max(m2 / 9.0 - mean * mean, 0.0));
    float3 history = current.rgb;
    float count = 0.0;
    float fast = currentLuma;
    float spread2 = 0.0;
    if (previous.Valid)
    {
        history = previous.Value;
        count = previous.Count;
        spread2 = previous.Spread2;
        // The fast luminance, clipped to the neighbourhood's range widened by its spread, takes
        // this frame's sample (at least a period's share) before it is compared.
        fast = clamp(previous.Fast, neighborMin.x - ClipSpread * sigma.x,
                     neighborMax.x + ClipSpread * sigma.x);
        fast = lerp(fast, currentLuma, max(share / (count + share), share / FastSamples));

        // Doubt the history as far as the fast luminance strays past its learnt spread.
        const float historyLuma = RgbToYCoCg(history).x;
        const float floorShare = lerp(LearningFloor, ChangeFloor, saturate(count / TrustSamples));
        const float tolerance = ChangeSpread * sqrt(max(spread2, 0.0)) +
                                floorShare * max(historyLuma, fast) + 1e-4;
        const float excess = abs(fast - historyLuma) / tolerance;
        count *= saturate(1.0 - (excess - 1.0) / ClipReset);
    }

    // The long count, or the samples spanning MovingReach pixels of the image's travel.
    const float travel = length(motion * float2(g_PC.Extent));
    const float maxCount = clamp(MovingReach / max(travel, 1e-6), MovingSamples, StillSamples);
    const float newCount = min(count + share, maxCount);
    const float alpha = share / max(newCount, share);
    const float3 resolved = lerp(history, current.rgb, alpha);

    // The fast luminance's spread about the history is learnt with the history's own share, so it
    // is the spread over the samples the history averaged.
    const float newFast = fast;
    const float stray = newFast - RgbToYCoCg(resolved).x;
    const float newSpread2 = lerp(spread2, stray * stray, alpha);

    output.History = float4(resolved, newCount);
    output.Fast = float2(newFast, newSpread2);
    output.Scene = float4(resolved, 1.0);
    return output;
}
