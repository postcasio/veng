// Depth-of-field fill: a small center-weighted 3x3 tent over one gathered layer. At a large
// circle of confusion the ring gather's fixed sample budget spreads thin enough to leave
// single-texel gaps between rings; averaging the immediate neighbourhood closes them without
// widening the bokeh, since the kernel is one texel across where the gather's is tens.
//
// Set 0 is reserved for the bindless registry; this stage binds the shared sampled-source +
// sampler + storage-destination set on set 1.
[[vk::binding(0, 3)]] Texture2D<float4> g_Source;
[[vk::binding(1, 3)]] SamplerState g_Sampler;
[[vk::binding(2, 3)]] [format("rgba16f")] RWTexture2D<float4> g_Dest;

struct PushConstants
{
    uint2 DestExtent;  // half-resolution valid extent in texels
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_PC.DestExtent.x || id.y >= g_PC.DestExtent.y)
        return;

    float2 texel = 1.0 / float2(g_PC.DestExtent);
    float2 uv = (float2(id.xy) + 0.5) * texel;

    float4 acc = float4(0.0, 0.0, 0.0, 0.0);
    float weightSum = 0.0;
    for (int y = -1; y <= 1; ++y)
    {
        for (int x = -1; x <= 1; ++x)
        {
            float w = (x == 0 && y == 0) ? 4.0 : 1.0;
            acc += g_Source.SampleLevel(g_Sampler, uv + float2(x, y) * texel, 0.0) * w;
            weightSum += w;
        }
    }

    g_Dest[int2(id.xy)] = acc / weightSum;
}
