// Gather fragment stage: samples one Presented viewport's texture and writes it
// into the assembly target. The owning pass sets the viewport and scissor to the
// placement's region, so the fullscreen triangle's v_UV runs 0..1 across the
// region and addresses the source texture's full extent. Point-sampled (the pass
// binds a nearest sampler), so a window-covering same-resolution placement is a
// bit-exact 1:1 copy. The placement texture and sampler are addressed by bindless
// handles in the constant block; set 0 is the bindless registry.
struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

struct PushConstants
{
    uint PlacementTexture;
    uint Sampler;
};

[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];
[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
    return g_Textures[NonUniformResourceIndex(g_PC.PlacementTexture)].Sample(
        g_Samplers[NonUniformResourceIndex(g_PC.Sampler)], input.v_UV);
}
