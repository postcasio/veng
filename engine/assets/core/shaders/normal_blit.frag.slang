// Normal-visualize fragment stage: samples the g-buffer world-normal target (G1)
// through the bindless set-0 array and writes a decoded view of it — the
// signed [-1, 1] world normal remapped to the [0, 1] display range
// (n * 0.5 + 0.5), the conventional normal-buffer debug visualization. A debug
// view of the deferred g-buffer; the background (a zero normal) maps to mid-grey.
[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];

struct PushConstants
{
    uint NormalTexture;
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
    float3 normal = g_Textures[NonUniformResourceIndex(g_PC.NormalTexture)].Sample(
        g_Samplers[NonUniformResourceIndex(g_PC.Sampler)], input.v_UV).xyz;
    return float4(normal * 0.5 + 0.5, 1.0);
}
