// Bloom upsample-accumulate: one destination mip per dispatch, sweeping from the
// coarsest level toward mip 0. Each invocation owns one texel of the finer destination
// level i: it samples the coarser level i+1 with a 3x3 tent (bilinear), scales by
// Radius, and adds the result into level i's existing content read-modify-write through
// the storage view (mip[i] += tent(mip[i+1]) * Radius).
//
// Reading and writing only the dispatch's own texel of level i is hazard-free across
// threads; the per-level barrier the graph derives orders this against the downsample
// that wrote level i and against the next finer up-step that reads it.
//
// Set 0 is the bindless registry; set 1 carries the closed resources: the sampled
// coarser source, the clamp-to-edge linear sampler, and the storage destination level.
[[vk::binding(0, 1)]] Texture2D<float4> g_Source;
[[vk::binding(1, 1)]] SamplerState g_Sampler;
[[vk::binding(2, 1)]] [format("rgba16f")] RWTexture2D<float4> g_Dest;

struct PushConstants
{
    uint2  DestExtent;     // destination (finer) mip dimensions in texels (the valid sub-rect)
    float2 SourceScaleUV;  // source validExtent/allocExtent (dynamic-resolution sub-rect)
    float2 SourceMaxUV;    // source (validExtent-0.5)/allocExtent (bilinear-tap clamp)
    float  Radius;         // upsample spread scale
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

// The source (coarser mip) is rendered into a dynamic-resolution sub-rect; map the valid-space UV
// into its valid region and clamp so a bilinear tap never bleeds past it (identity at scale 1.0).
float3 SampleSource(float2 uv)
{
    float2 physical = min(uv * g_PC.SourceScaleUV, g_PC.SourceMaxUV);
    return g_Source.SampleLevel(g_Sampler, physical, 0.0).rgb;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_PC.DestExtent.x || id.y >= g_PC.DestExtent.y)
        return;

    float2 texel = 1.0 / float2(g_PC.DestExtent);
    float2 uv = (float2(id.xy) + 0.5) * texel;

    // A 3x3 tent over the coarser source, in destination-texel offsets (the classic COD
    // upsample filter): center weight 4, edges 2, corners 1, normalized by 16.
    float2 o = texel;
    float3 sum = SampleSource(uv + float2(-1, -1) * o) * 1.0;
    sum += SampleSource(uv + float2(0, -1) * o) * 2.0;
    sum += SampleSource(uv + float2(1, -1) * o) * 1.0;
    sum += SampleSource(uv + float2(-1, 0) * o) * 2.0;
    sum += SampleSource(uv + float2(0, 0) * o) * 4.0;
    sum += SampleSource(uv + float2(1, 0) * o) * 2.0;
    sum += SampleSource(uv + float2(-1, 1) * o) * 1.0;
    sum += SampleSource(uv + float2(0, 1) * o) * 2.0;
    sum += SampleSource(uv + float2(1, 1) * o) * 1.0;
    sum *= (1.0 / 16.0);

    float3 existing = g_Dest[int2(id.xy)].rgb;
    g_Dest[int2(id.xy)] = float4(existing + sum * g_PC.Radius, 1.0);
}
