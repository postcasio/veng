// TAA history-copy fragment stage: samples the resolved HDR target and writes it,
// unclamped, into the persisted history target so the next frame can reproject
// against it. Distinct from hdr_blit, which saturates to [0, 1] for display — the
// history must keep its full HDR range, since the resolve blends in linear HDR.
[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];

struct PushConstants
{
    uint SourceTexture; // the resolved HDR target (bindless index)
    uint Sampler;       // shared sampler
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target0
{
    const float3 resolved = g_Textures[NonUniformResourceIndex(g_PC.SourceTexture)].Sample(
        g_Samplers[NonUniformResourceIndex(g_PC.Sampler)], input.v_UV).rgb;
    return float4(resolved, 1.0);
}
