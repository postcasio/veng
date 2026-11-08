// Gather fragment stage: samples one Presented viewport's texture and writes it
// into the assembly target. The owning pass sets the viewport and scissor to the
// placement's region, so the fullscreen triangle's v_UV runs 0..1 across the
// region and addresses the source texture's full extent. The pass binds a linear,
// clamp-to-edge sampler, so a same-resolution placement samples at texel centres
// (a bit-exact 1:1 copy) and a larger or smaller one is filtered into its region.
// The placement texture and sampler are addressed by bindless handles in the
// constant block; set 0 is the bindless registry.
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

#include "Veng/bindless.slang"
VE_PUSH_CONSTANTS(PushConstants, g_PC);

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
    return g_Textures[NonUniformIndex(g_PC.PlacementTexture)].Sample(
        g_Samplers[NonUniformIndex(g_PC.Sampler)], input.v_UV);
}
