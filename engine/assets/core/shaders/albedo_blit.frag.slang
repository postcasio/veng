// Albedo-blit fragment stage: samples the g-buffer albedo target (G0) through
// the bindless set-0 array, selected by the texture/sampler indices in the push
// block, and writes it straight to the output — an unlit albedo view of the
// scene. G0 is an sRGB target, so the sample decodes to linear; writing it to an
// RGBA16F linear output keeps it linear (the swapchain composite handles final
// display encoding).
[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];

struct PushConstants
{
    uint AlbedoTexture;
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
    float4 albedo = g_Textures[NonUniformResourceIndex(g_PC.AlbedoTexture)].Sample(
        g_Samplers[NonUniformResourceIndex(g_PC.Sampler)], input.v_UV);
    return float4(albedo.rgb, 1.0);
}
