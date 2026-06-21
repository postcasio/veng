// Bloom composite: one dispatch over the full-resolution output. Each invocation owns
// one result texel, samples the lit HDR target and the accumulated bloom (mip 0 of the
// pyramid), and stores hdr + bloom * Intensity into the result image the tonemap reads.
// Compositing in linear HDR keeps bloom ahead of tone mapping.
//
// Set 0 is the bindless registry; set 1 carries the closed composite resources: two
// sampled inputs (HDR at binding 0, bloom mip 0 at binding 1), the clamp-to-edge linear
// sampler, and the storage result at binding 3.
[[vk::binding(0, 1)]] Texture2D<float4> g_Hdr;
[[vk::binding(1, 1)]] Texture2D<float4> g_Bloom;
[[vk::binding(2, 1)]] SamplerState g_Sampler;
[[vk::binding(3, 1)]] [format("rgba16f")] RWTexture2D<float4> g_Dest;

struct PushConstants
{
    uint2 DestExtent;  // result dimensions in texels
    float Intensity;   // bloom mix
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_PC.DestExtent.x || id.y >= g_PC.DestExtent.y)
        return;

    float2 uv = (float2(id.xy) + 0.5) / float2(g_PC.DestExtent);

    float3 hdr = g_Hdr.SampleLevel(g_Sampler, uv, 0.0).rgb;
    float3 bloom = g_Bloom.SampleLevel(g_Sampler, uv, 0.0).rgb;

    g_Dest[int2(id.xy)] = float4(hdr + bloom * g_PC.Intensity, 1.0);
}
