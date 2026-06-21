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
// Set 0 is reserved across every pipeline layout for the bindless registry; this closed
// producer->consumer chain binds its own resources on set 1: a sampled source mip, the
// clamp-to-edge linear sampler, and a single storage destination mip.
[[vk::binding(0, 1)]] Texture2D<float4> g_Source;
[[vk::binding(1, 1)]] SamplerState g_Sampler;
[[vk::binding(2, 1)]] [format("rgba16f")] RWTexture2D<float4> g_Dest;

struct PushConstants
{
    uint2 DestExtent;    // destination mip dimensions in texels
    float BrightPass;    // 1.0 = level-0 bright-pass + Karis, 0.0 = plain downsample
    float Threshold;     // bright-pass luminance knee (level 0 only)
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

float3 SampleSource(float2 uv)
{
    return g_Source.SampleLevel(g_Sampler, uv, 0.0).rgb;
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
        result *= soft * soft;
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
