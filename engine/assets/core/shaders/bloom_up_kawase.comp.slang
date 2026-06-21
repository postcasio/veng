// Bloom upsample-accumulate (Dual Kawase): one destination mip per dispatch, sweeping from
// the coarsest level toward mip 0. Each invocation owns one texel of the finer destination
// level i: it samples the coarser level i+1 with the Dual Kawase 8-tap bilinear pattern
// (four edge taps weighted 2, four corner taps weighted 1, normalized by 12), scales by
// Radius, and adds the result into level i's existing content read-modify-write through the
// storage view (mip[i] += kawase(mip[i+1]) * Radius).
//
// Reading and writing only the dispatch's own texel of level i is hazard-free across
// threads; the per-level barrier the graph derives orders this against the downsample that
// wrote level i and against the next finer up-step that reads it.
//
// Set 0 is the bindless registry; set 1 carries the closed resources: the sampled coarser
// source, the clamp-to-edge linear sampler, and the storage destination level.
[[vk::binding(0, 1)]] Texture2D<float4> g_Source;
[[vk::binding(1, 1)]] SamplerState g_Sampler;
[[vk::binding(2, 1)]] [format("rgba16f")] RWTexture2D<float4> g_Dest;

struct PushConstants
{
    uint2 DestExtent;  // destination (finer) mip dimensions in texels
    float Radius;      // upsample spread scale
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

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

    // The Dual Kawase 8-tap: four edge taps a half-texel out (weight 2) and four corner taps
    // a full texel out on the diagonals (weight 1), normalized by 12. Offsets are in
    // destination texels; the bilinear fetches widen each tap's footprint.
    float2 h = texel * 0.5;
    float3 sum = SampleSource(uv + float2(-1, 0) * h) * 2.0;
    sum += SampleSource(uv + float2(1, 0) * h) * 2.0;
    sum += SampleSource(uv + float2(0, -1) * h) * 2.0;
    sum += SampleSource(uv + float2(0, 1) * h) * 2.0;
    sum += SampleSource(uv + float2(-1, -1) * texel) * 1.0;
    sum += SampleSource(uv + float2(1, -1) * texel) * 1.0;
    sum += SampleSource(uv + float2(-1, 1) * texel) * 1.0;
    sum += SampleSource(uv + float2(1, 1) * texel) * 1.0;
    sum *= (1.0 / 12.0);

    float3 existing = g_Dest[int2(id.xy)].rgb;
    g_Dest[int2(id.xy)] = float4(existing + sum * g_PC.Radius, 1.0);
}
