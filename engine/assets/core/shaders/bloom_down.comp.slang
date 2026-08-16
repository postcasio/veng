// Bloom downsample: one destination mip per dispatch. Each invocation owns one
// destination texel and gathers a 13-tap Call-of-Duty/Jimenez kernel from the source
// (the lit HDR target for mip 0, the prior bloom mip otherwise), reading bilinearly so
// the wide taps land between source texels.
//
// Mip 0 fuses two extra steps: a soft-knee bright-pass (the luminance above Threshold
// ramps in continuously) and a Karis partial-tonemap average (each 2x2 tap group is
// weighted by 1/(1+luma)) so a single firefly pixel cannot drive a flickering bloom.
// Deeper levels use the plain 13-tap average. A push flag selects the path.
//
// Mip 0 also takes the larger of that luminance bright-pass and the authored bloom mask, a
// screen-space target a forward material writes alongside its color to name the glow it wants
// independently of how bright it is. The mask is gathered through the same kernel and the same
// Karis weights as the color, so the two stay in register: a stroke thin enough to be averaged
// down by the 13 taps has its mask averaged down by exactly as much. The seeded pyramid still
// carries the filtered scene *color*, so a masked surface halos in its own hue rather than in
// the white its radiance would have to reach to cross the knee.
//
// The mask is an **amplitude**: it multiplies the filtered color, so 1.0 seeds the pyramid with
// the surface's own radiance and anything above 1.0 seeds it brighter. The bright-pass gain it
// is maxed against saturates at 1.0; the mask deliberately does not, which is why its target is
// a float format and why nothing here clamps it.
//
// Set 0 is reserved across every pipeline layout for the bindless registry; this closed
// producer->consumer chain binds its own resources on set 1: a sampled source mip, the
// clamp-to-edge linear sampler, and a single storage destination mip. The mask is the one
// resource read off the registry instead — it belongs to the renderer, not to this chain.
[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];

[[vk::binding(0, 3)]] Texture2D<float4> g_Source;
[[vk::binding(1, 3)]] SamplerState g_Sampler;
[[vk::binding(2, 3)]] [format("rgba16f")] RWTexture2D<float4> g_Dest;

struct PushConstants
{
    uint2  DestExtent;     // destination mip dimensions in texels (the valid sub-rect)
    float2 SourceScaleUV;  // source validExtent/allocExtent (dynamic-resolution sub-rect)
    float2 SourceMaxUV;    // source (validExtent-0.5)/allocExtent (bilinear-tap clamp)
    float  BrightPass;     // 1.0 = level-0 bright-pass + Karis, 0.0 = plain downsample
    float  Threshold;      // bright-pass luminance knee (level 0 only)
    uint   MaskTexture;    // bindless slot of the bloom-mask target (level 0 only)
    uint   MaskSampler;    // bindless slot of the sampler the mask is read through
    uint   MaskEnabled;    // 1 = fold the mask in, 0 = the two slots above are unset
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

// Rec. 709 relative luminance.
float Luma(float3 c)
{
    return dot(c, float3(0.2126, 0.7152, 0.0722));
}

// Karis weight: a partial-tonemap that down-weights bright outliers, applied per 2x2
// tap group on the first downsample so one firefly cannot dominate the average.
float KarisWeight(float3 c)
{
    return 1.0 / (1.0 + Luma(c));
}

// The source is rendered into a dynamic-resolution sub-rect; map the valid-space UV into the
// source's valid region and clamp so a bilinear tap never bleeds past it (identity at scale 1.0).
float3 SampleSource(float2 uv)
{
    float2 physical = min(uv * g_PC.SourceScaleUV, g_PC.SourceMaxUV);
    return g_Source.SampleLevel(g_Sampler, physical, 0.0).rgb;
}

// The bloom mask at the same valid-space UV as SampleSource, through the same sub-rect mapping:
// the mask target is written by the same pass that wrote the color, at the same resolution.
float SampleMask(float2 uv)
{
    float2 physical = min(uv * g_PC.SourceScaleUV, g_PC.SourceMaxUV);
    return g_Textures[g_PC.MaskTexture]
        .SampleLevel(g_Samplers[g_PC.MaskSampler], physical, 0.0)
        .r;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_PC.DestExtent.x || id.y >= g_PC.DestExtent.y)
        return;

    // The destination texel center in [0,1]; one source texel is two destination texels
    // wide, so the kernel offsets below (in destination texels) sample at source-texel
    // half steps for the bilinear COD pattern.
    float2 texel = 1.0 / float2(g_PC.DestExtent);
    float2 uv = (float2(id.xy) + 0.5) * texel;

    // The 13-tap kernel: a center, an inner 2x2 ring and an outer 3x3 ring of the
    // COD/Jimenez "Next Generation Post Processing" downsample. Offsets are in
    // destination texels (== half a source texel each side).
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
        // Karis-averaged 2x2 groups: weight each group by 1/(1+luma(group mean)) so a
        // single bright outlier is suppressed before the chain accumulates it.
        float3 g0 = (j + k + l + m) * 0.25; // inner ring
        float3 g1 = (a + b + d + e) * 0.25; // top-left
        float3 g2 = (b + c + e + f) * 0.25; // top-right
        float3 g3 = (d + e + g + h) * 0.25; // bottom-left
        float3 g4 = (e + f + h + i) * 0.25; // bottom-right

        float w0 = KarisWeight(g0);
        float w1 = KarisWeight(g1);
        float w2 = KarisWeight(g2);
        float w3 = KarisWeight(g3);
        float w4 = KarisWeight(g4);
        float wsum = w0 + w1 + w2 + w3 + w4;

        result = (g0 * w0 + g1 * w1 + g2 * w2 + g3 * w3 + g4 * w4) / wsum;

        // Soft-knee bright-pass: a smooth quadratic ramp over a Threshold-wide band so a
        // surface near the knee fades in rather than popping (the standard shape).
        float knee = max(g_PC.Threshold, 1e-4);
        float soft = saturate((Luma(result) - knee) / knee);
        float gain = soft * soft;

        if (g_PC.MaskEnabled != 0)
        {
            // The mask through the same 13 taps, the same 2x2 groups and the same Karis
            // weights the color took, so the two are the same average of the same footprint.
            float ma = SampleMask(uv + float2(-2, -2) * o);
            float mb = SampleMask(uv + float2(0, -2) * o);
            float mc = SampleMask(uv + float2(2, -2) * o);
            float md = SampleMask(uv + float2(-2, 0) * o);
            float me = SampleMask(uv + float2(0, 0) * o);
            float mf = SampleMask(uv + float2(2, 0) * o);
            float mg = SampleMask(uv + float2(-2, 2) * o);
            float mh = SampleMask(uv + float2(0, 2) * o);
            float mi = SampleMask(uv + float2(2, 2) * o);
            float mj = SampleMask(uv + float2(-1, -1) * o);
            float mk = SampleMask(uv + float2(1, -1) * o);
            float ml = SampleMask(uv + float2(-1, 1) * o);
            float mm = SampleMask(uv + float2(1, 1) * o);

            float m0 = (mj + mk + ml + mm) * 0.25;
            float m1 = (ma + mb + md + me) * 0.25;
            float m2 = (mb + mc + me + mf) * 0.25;
            float m3 = (md + me + mg + mh) * 0.25;
            float m4 = (me + mf + mh + mi) * 0.25;

            float mask = (m0 * w0 + m1 * w1 + m2 * w2 + m3 * w3 + m4 * w4) / wsum;
            gain = max(gain, mask);
        }

        result *= gain;
    }
    else
    {
        // The plain COD 13-tap weights (the five 2x2 groups, inner ring at 0.5 total).
        float3 inner = (j + k + l + m) * 0.25;
        float3 tl = (a + b + d + e) * 0.25;
        float3 tr = (b + c + e + f) * 0.25;
        float3 bl = (d + e + g + h) * 0.25;
        float3 br = (e + f + h + i) * 0.25;
        result = inner * 0.5 + (tl + tr + bl + br) * 0.125;
    }

    g_Dest[int2(id.xy)] = float4(result, 1.0);
}
