// Bloom downsample (Dual Kawase): one destination mip per dispatch. Each invocation owns
// one destination texel and gathers a 5-tap bilinear pattern from the source (the lit HDR
// target for mip 0, the prior bloom mip otherwise) — a center tap plus four diagonal
// half-texel taps. The bilinear fetches let five samples cover the same footprint the COD
// 13-tap covers with more taps, for less bandwidth on the tile-based GPUs this kernel targets.
// Every level halves its source — mip 0 is half the scene colour — so the bright pass is the
// chain's first 2:1 downsample.
//
// Mip 0 fuses two extra steps: a per-tap brightness ceiling (bloom_bright_pass.slang) applied to
// each tap before the plain 5-tap average, and a soft-knee bright-pass on that average (the
// luminance above Threshold ramps in continuously). The average stays linear for every tap under
// the ceiling, so a small bright source seeds the same light wherever it falls on the texel grid,
// while one extreme texel seeds no more than the ceiling. Deeper levels are the plain 5-tap
// average. A push flag selects the path; the ceiling and the bright-pass wrap the kernel rather
// than the reverse, so both bloom kernels share a stable mip 0.
//
// Mip 0 also takes the larger of that luminance bright-pass and the authored bloom mask, a
// screen-space target a forward material writes alongside its color to name the glow it wants
// independently of how bright it is. The mask is gathered through the same taps and the same
// weights as the color, so the two stay in register, and the pyramid is still seeded with the
// filtered scene *color* — a masked surface halos in its own hue.
//
// Set 0 is reserved across every pipeline layout for the bindless registry; this closed
// producer->consumer chain binds its own resources on set 3, past the typed bindless registries:
// a sampled source mip, the clamp-to-edge linear sampler, and a single storage destination mip.
// The mask is the one resource read off the registry instead — it belongs to the renderer, not to
// this chain.
#include "bloom_bright_pass.slang"

[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];

[[vk::binding(0, 3)]] Texture2D<float4> g_Source;
[[vk::binding(1, 3)]] SamplerState g_Sampler;
[[vk::binding(2, 3)]] [format("rgba16f")] RWTexture2D<float4> g_Dest;

// The source is rendered into a dynamic-resolution sub-rect; map the valid-space UV into the
// source's valid region and clamp so a bilinear tap never bleeds past it (identity at scale 1.0).
float3 SampleSource(float2 uv)
{
    float2 physical = min(uv * g_PC.SourceScaleUV, g_PC.SourceMaxUV);
    return g_Source.SampleLevel(g_Sampler, physical, 0.0).rgb;
}

// The bloom mask reaches this dispatch in the same allocation as the colour beside it — promoted
// from the rendered sub-rect alongside the scene colour when the scene rasterized less than it — so
// the colour's sub-rect mapping serves it too.
float SampleMask(float2 uv)
{
    float2 physical = min(uv * g_PC.SourceScaleUV, g_PC.SourceMaxUV);
    return g_Textures[g_PC.MaskTexture]
        .SampleLevel(g_Samplers[g_PC.MaskSampler], physical, 0.0)
        .r;
}

// The plain Kawase weights: center 0.5, each diagonal 0.125 (sum 1).
float3 KawaseAverage(float3 center, float3 tl, float3 tr, float3 bl, float3 br)
{
    return center * 0.5 + (tl + tr + bl + br) * 0.125;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_PC.DestExtent.x || id.y >= g_PC.DestExtent.y)
        return;

    float2 texel = 1.0 / float2(g_PC.DestExtent);
    float2 uv = (float2(id.xy) + 0.5) * texel;

    // The Dual Kawase 5-tap: a center bilinear fetch and four diagonal taps a half-texel
    // out (in destination texels), each bilinear so it averages a 2x2 source neighborhood.
    float2 o = texel * 0.5;
    float3 center = SampleSource(uv);
    float3 tl = SampleSource(uv + float2(-1, -1) * o);
    float3 tr = SampleSource(uv + float2(1, -1) * o);
    float3 bl = SampleSource(uv + float2(-1, 1) * o);
    float3 br = SampleSource(uv + float2(1, 1) * o);

    float3 result;
    if (g_PC.BrightPass != 0.0)
    {
        // Each tap is conditioned on its own before the average (see bloom_down.comp).
        result = KawaseAverage(ConditionTap(center), ConditionTap(tl), ConditionTap(tr),
                               ConditionTap(bl), ConditionTap(br));

        // Soft-knee bright-pass: a smooth quadratic ramp over a Threshold-wide band so a
        // surface near the knee fades in rather than popping (the standard shape).
        float knee = max(g_PC.Threshold, 1e-4);
        float soft = saturate((Luma(result) - knee) / knee);
        float gain = soft * soft;

        if (g_PC.MaskEnabled != 0)
        {
            // The mask through the same five taps and the same weights the color took.
            float mc = ConditionMask(SampleMask(uv));
            float mtl = ConditionMask(SampleMask(uv + float2(-1, -1) * o));
            float mtr = ConditionMask(SampleMask(uv + float2(1, -1) * o));
            float mbl = ConditionMask(SampleMask(uv + float2(-1, 1) * o));
            float mbr = ConditionMask(SampleMask(uv + float2(1, 1) * o));

            float mask = KawaseAverage(mc.xxx, mtl.xxx, mtr.xxx, mbl.xxx, mbr.xxx).x;
            gain = max(gain, mask);
        }

        result *= gain;
    }
    else
    {
        result = KawaseAverage(center, tl, tr, bl, br);
    }

    g_Dest[int2(id.xy)] = float4(result, 1.0);
}
