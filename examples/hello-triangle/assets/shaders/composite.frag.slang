// Composite fragment stage: blends the ImGui overlay over the rendered scene.
// The scene and overlay images, plus the shared sampler, are addressed by
// bindless handles pushed in the constant block; set 0 is the bindless registry.
struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

struct PushConstants
{
    uint SceneTexture;
    uint ImGuiTexture;
    uint Sampler;
};

[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];
[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
    float4 scene = g_Textures[NonUniformResourceIndex(g_PC.SceneTexture)].Sample(
        g_Samplers[NonUniformResourceIndex(g_PC.Sampler)], input.v_UV);
    float4 ui = g_Textures[NonUniformResourceIndex(g_PC.ImGuiTexture)].Sample(
        g_Samplers[NonUniformResourceIndex(g_PC.Sampler)], input.v_UV);
    return float4(lerp(scene.rgb, ui.rgb, ui.a), 1.0);
}
