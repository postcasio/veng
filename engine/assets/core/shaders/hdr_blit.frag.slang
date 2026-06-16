// HDR-blit fragment stage: samples the HDR lighting target through the bindless
// set-0 array and writes it (clamped to [0, 1]) to the output. The stand-in tail
// of the deferred chain — a tonemap pass replaces it, mapping HDR → output with a
// real curve. The clamp keeps an out-of-range HDR value from wrapping the output
// while the chain has no tonemap.
[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];

struct PushConstants
{
    uint HdrTexture;
    uint Sampler;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
    float3 hdr = g_Textures[NonUniformResourceIndex(g_PC.HdrTexture)].Sample(
        g_Samplers[NonUniformResourceIndex(g_PC.Sampler)], input.v_UV).rgb;
    return float4(saturate(hdr), 1.0);
}
