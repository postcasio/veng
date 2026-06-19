// Shadow-visualize fragment stage: samples the directional shadow map (the
// ShadowScenePass's depth target, the scene's depth from the light's orthographic
// view) through the bindless set-0 array and writes a greyscale view of its stored
// caster depth. Near-to-light casters read dark, far reads bright (1 - depth) — the
// conventional shadow-map debug view. The g-buffer depth and this shadow map are
// the two depth targets read as textures in the engine.
[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];

struct PushConstants
{
    uint ShadowTexture;
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
    float depth = g_Textures[NonUniformResourceIndex(g_PC.ShadowTexture)].Sample(
        g_Samplers[NonUniformResourceIndex(g_PC.Sampler)], input.v_UV).r;
    float shade = saturate(1.0 - depth);
    return float4(shade, shade, shade, 1.0);
}
