// Shadow-atlas visualize fragment stage: samples the directional shadow atlas (the
// ShadowScenePass's depth target, the scene's depth from the light's view per
// cascade tile) and writes a greyscale view of its stored caster depth.
// The atlas is reverse-Z (near-to-light = 1, far = 0), so the raw depth writes
// straight through and near-to-light casters read bright — the conventional
// shadow-map debug view. The atlas is off bindless, so it reaches this blit through
// a dedicated set (set 1): an ordinary (non-comparison) sampler so the raw stored
// depth is returned, not a compare result.

#include "Veng/binding.slang"
struct ShadowBlitSet3
{
    Texture2D ShadowAtlas;
    SamplerState ShadowSampler;
};
VE_DESCRIPTOR_SET(3, ShadowBlitSet3, g_Set3);

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
    float depth = g_Set3.ShadowAtlas.Sample(g_Set3.ShadowSampler, input.v_UV).r;
    float shade = saturate(depth);
    return float4(shade, shade, shade, 1.0);
}
