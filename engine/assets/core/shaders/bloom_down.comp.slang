// Bloom downsample: one destination mip per dispatch. Each invocation owns one
// destination texel and gathers a 13-tap Call-of-Duty/Jimenez kernel from the source
// (the lit HDR target for mip 0, the prior bloom mip otherwise), reading bilinearly so
// the wide taps land between source texels. Every level halves its source — mip 0 is half
// the scene colour — so the bright pass is the chain's first 2:1 downsample.
//
// Mip 0 fuses two extra steps: a per-tap brightness ceiling (bloom_bright_pass.slang) applied to
// each tap before the plain 13-tap average, and a soft-knee bright-pass on that average (the
// luminance above Threshold ramps in continuously). The average stays linear for every tap under
// the ceiling, so a small bright source seeds the same light wherever it falls on the texel grid,
// while one extreme texel seeds no more than the ceiling. Deeper levels are the plain 13-tap
// average. A push flag selects the path.
//
// Mip 0 also takes the larger of that luminance bright-pass and the authored bloom mask, a
// screen-space target a forward material writes alongside its color to name the glow it wants
// independently of how bright it is. The mask is gathered through the same kernel and the same
// weights as the color, so the two stay in register: a stroke thin enough to be averaged down by
// the 13 taps has its mask averaged down by exactly as much. The seeded pyramid still carries the
// filtered scene *color*, so a masked surface halos in its own hue rather than in the white its
// radiance would have to reach to cross the knee.
//
// The mask is an **amplitude**: it multiplies the filtered color, so 1.0 seeds the pyramid with
// the surface's own radiance and anything above 1.0 seeds it brighter. The bright-pass gain it
// is maxed against saturates at 1.0; the mask deliberately does not, which is why its target is
// a float format and why nothing here clamps it.
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

// The plain COD 13-tap weights: the five 2x2 groups, the inner ring at 0.5 and each corner group
// at 0.125. Taps a..i are the outer 3x3 grid in row order, j..m the inner ring.
float3 CodAverage(float3 a, float3 b, float3 c, float3 d, float3 e, float3 f, float3 g, float3 h,
                  float3 i, float3 j, float3 k, float3 l, float3 m)
{
    float3 inner = (j + k + l + m) * 0.25;
    float3 tl = (a + b + d + e) * 0.25;
    float3 tr = (b + c + e + f) * 0.25;
    float3 bl = (d + e + g + h) * 0.25;
    float3 br = (e + f + h + i) * 0.25;
    return inner * 0.5 + (tl + tr + bl + br) * 0.125;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_PC.DestExtent.x || id.y >= g_PC.DestExtent.y)
        return;

    // The destination texel center in [0,1]. Every level, mip 0 included, halves its source,
    // so one destination texel spans two source texels; the kernel offsets below are in
    // destination texels.
    float2 texel = 1.0 / float2(g_PC.DestExtent);
    float2 uv = (float2(id.xy) + 0.5) * texel;

    // The 13-tap kernel: a center, an inner 2x2 ring and an outer 3x3 ring of the
    // COD/Jimenez "Next Generation Post Processing" downsample. Offsets are in
    // destination texels (== two source texels each).
    float2 o = texel;

    float3 a = SampleSource(uv + float2(-2, -2) * o);
    float3 b = SampleSource(uv + float2(0, -2) * o);
    float3 c = SampleSource(uv + float2(2, -2) * o);
    float3 d = SampleSource(uv + float2(-2, 0) * o);
    float3 e = SampleSource(uv + float2(0, 0) * o);
    float3 f = SampleSource(uv + float2(2, 0) * o);
    float3 g = SampleSource(uv + float2(-2, 2) * o);
    float3 h = SampleSource(uv + float2(0, 2) * o);
    float3 i = SampleSource(uv + float2(2, 2) * o);
    float3 j = SampleSource(uv + float2(-1, -1) * o);
    float3 k = SampleSource(uv + float2(1, -1) * o);
    float3 l = SampleSource(uv + float2(-1, 1) * o);
    float3 m = SampleSource(uv + float2(1, 1) * o);

    float3 result;
    if (g_PC.BrightPass != 0.0)
    {
        // Each tap is conditioned on its own before the average, so how a small source divides
        // among the taps cannot change what it seeds while every tap stays under the ceiling.
        result = CodAverage(ConditionTap(a), ConditionTap(b), ConditionTap(c), ConditionTap(d),
                            ConditionTap(e), ConditionTap(f), ConditionTap(g), ConditionTap(h),
                            ConditionTap(i), ConditionTap(j), ConditionTap(k), ConditionTap(l),
                            ConditionTap(m));

        // Soft-knee bright-pass: a smooth quadratic ramp over a Threshold-wide band so a
        // surface near the knee fades in rather than popping (the standard shape).
        float knee = max(g_PC.Threshold, 1e-4);
        float soft = saturate((Luma(result) - knee) / knee);
        float gain = soft * soft;

        if (g_PC.MaskEnabled != 0)
        {
            // The mask through the same 13 taps and the same weights the color took, so the two
            // are the same average of the same footprint.
            float ma = ConditionMask(SampleMask(uv + float2(-2, -2) * o));
            float mb = ConditionMask(SampleMask(uv + float2(0, -2) * o));
            float mc = ConditionMask(SampleMask(uv + float2(2, -2) * o));
            float md = ConditionMask(SampleMask(uv + float2(-2, 0) * o));
            float me = ConditionMask(SampleMask(uv + float2(0, 0) * o));
            float mf = ConditionMask(SampleMask(uv + float2(2, 0) * o));
            float mg = ConditionMask(SampleMask(uv + float2(-2, 2) * o));
            float mh = ConditionMask(SampleMask(uv + float2(0, 2) * o));
            float mi = ConditionMask(SampleMask(uv + float2(2, 2) * o));
            float mj = ConditionMask(SampleMask(uv + float2(-1, -1) * o));
            float mk = ConditionMask(SampleMask(uv + float2(1, -1) * o));
            float ml = ConditionMask(SampleMask(uv + float2(-1, 1) * o));
            float mm = ConditionMask(SampleMask(uv + float2(1, 1) * o));

            float mask = CodAverage(ma.xxx, mb.xxx, mc.xxx, md.xxx, me.xxx, mf.xxx, mg.xxx,
                                    mh.xxx, mi.xxx, mj.xxx, mk.xxx, ml.xxx, mm.xxx)
                             .x;
            gain = max(gain, mask);
        }

        result *= gain;
    }
    else
    {
        result = CodAverage(a, b, c, d, e, f, g, h, i, j, k, l, m);
    }

    g_Dest[int2(id.xy)] = float4(result, 1.0);
}
