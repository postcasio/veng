// Samples a bindless-registered texture/sampler pair selected by push-constant
// indices. The set-0 bindless arrays mirror the BindlessRegistry layout
// (sampled images = 0, samplers = 1) and are excluded from reflection.
[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];

struct PushConstants
{
    uint TextureIndex;
    uint SamplerIndex;
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
    return g_Textures[NonUniformResourceIndex(g_PC.TextureIndex)].Sample(
        g_Samplers[NonUniformResourceIndex(g_PC.SamplerIndex)], input.v_UV);
}
