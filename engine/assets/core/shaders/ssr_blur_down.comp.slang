// SSR reflection blur downsample: one destination mip per dispatch. Each invocation
// owns one destination texel and averages a 2x2 bilinear footprint of the source mip
// (the trace's sharp mip 0 for level 1, the prior blurred mip otherwise), building the
// roughness mip chain the SSR composite samples by LOD — a coarser mip is a blurrier
// reflection. The reflection's confidence rides the alpha channel and is averaged with
// the color, so a coarse (rough) mip fades where few rays hit.
//
// Set 0 is reserved for the bindless registry; this closed producer->consumer chain
// binds its own resources on set 1: a sampled source mip, a clamp-to-edge linear
// sampler, and a single storage destination mip (the bloom down/up set layout).
[[vk::binding(0, 1)]] Texture2D<float4> g_Source;
[[vk::binding(1, 1)]] SamplerState g_Sampler;
[[vk::binding(2, 1)]] [format("rgba16f")] RWTexture2D<float4> g_Dest;

struct PushConstants
{
    uint2 DestExtent;  // destination mip dimensions in texels
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_PC.DestExtent.x || id.y >= g_PC.DestExtent.y)
        return;

    // The source is twice the destination resolution; four bilinear taps a quarter of
    // a destination texel from the center cover the source's 2x2 footprint.
    float2 ts = 1.0 / float2(g_PC.DestExtent);
    float2 uv = (float2(id.xy) + 0.5) * ts;

    float4 c = g_Source.SampleLevel(g_Sampler, uv + ts * float2(-0.25, -0.25), 0.0);
    c += g_Source.SampleLevel(g_Sampler, uv + ts * float2(0.25, -0.25), 0.0);
    c += g_Source.SampleLevel(g_Sampler, uv + ts * float2(-0.25, 0.25), 0.0);
    c += g_Source.SampleLevel(g_Sampler, uv + ts * float2(0.25, 0.25), 0.0);

    g_Dest[int2(id.xy)] = c * 0.25;
}
