// Shadow-atlas visualize fragment stage: samples the directional shadow atlas (the
// ShadowScenePass's depth target, the scene's depth from the light's view per
// cascade tile) and writes a greyscale view of its stored caster depth.
// Near-to-light casters read dark, far reads bright (1 - depth) — the conventional
// shadow-map debug view. The atlas is off bindless, so it reaches this blit through
// a dedicated set (set 1): an ordinary (non-comparison) sampler so the raw stored
// depth is returned, not a compare result.
[[vk::binding(0, 1)]] Texture2D g_ShadowAtlas;
[[vk::binding(1, 1)]] SamplerState g_ShadowSampler;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
    float depth = g_ShadowAtlas.Sample(g_ShadowSampler, input.v_UV).r;
    float shade = saturate(1.0 - depth);
    return float4(shade, shade, shade, 1.0);
}
