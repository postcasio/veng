// Bloom downsample (Dual Kawase): one destination mip per dispatch. Each invocation owns
// one destination texel and gathers a 5-tap bilinear pattern from the source (the lit HDR
// target for mip 0, the prior bloom mip otherwise) — a center tap plus four diagonal
// half-texel taps. The bilinear fetches let five samples cover the same footprint the COD
// 13-tap covers with more taps, for less bandwidth on the tile-based GPUs this kernel targets.
//
// Mip 0 fuses two extra steps: a soft-knee bright-pass (the luminance above Threshold
// ramps in continuously) and a Karis partial-tonemap average (each diagonal tap is weighted
// by 1/(1+luma)) so a single firefly pixel cannot drive a flickering bloom. Deeper levels use
// the plain 5-tap average. A push flag selects the path; the bright-pass + Karis wrap the
// kernel rather than the reverse, so both bloom kernels share a stable mip 0.
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

// Karis weight: a partial-tonemap that down-weights bright outliers, applied per tap on the
// first downsample so one firefly cannot dominate the average.
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
        // Karis-averaged taps: weight each by 1/(1+luma) so a single bright outlier is
        // suppressed before the chain accumulates it.
        float wc = KarisWeight(center);
        float wtl = KarisWeight(tl);
        float wtr = KarisWeight(tr);
        float wbl = KarisWeight(bl);
        float wbr = KarisWeight(br);
        float wsum = wc + wtl + wtr + wbl + wbr;

        result = (center * wc + tl * wtl + tr * wtr + bl * wbl + br * wbr) / wsum;

        // Soft-knee bright-pass: a smooth quadratic ramp over a Threshold-wide band so a
        // surface near the knee fades in rather than popping (the standard shape).
        float knee = max(g_PC.Threshold, 1e-4);
        float soft = saturate((Luma(result) - knee) / knee);
        result *= soft * soft;
    }
    else
    {
        // The plain Kawase weights: center 0.5, each diagonal 0.125 (sum 1).
        result = center * 0.5 + (tl + tr + bl + br) * 0.125;
    }

    g_Dest[int2(id.xy)] = float4(result, 1.0);
}
