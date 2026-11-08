// ORM-channel-visualize fragment stage: samples the packed g-buffer ORM target
// (G2 — R occlusion, G roughness, B metallic, A surface flags) through the
// bindless set-0 array and writes one channel as greyscale, selected by the
// Channel index in the push block (0 = occlusion, 1 = roughness, 2 = metallic;
// the flags channel has no greyscale reading). The debug views for the three
// material channels share this one shader, differing only in the pushed Channel.
#include "Veng/bindless.slang"

struct PushConstants
{
    uint OrmTexture;
    uint Sampler;
    uint Channel; // 0 occlusion (R), 1 roughness (G), 2 metallic (B)
};

VE_PUSH_CONSTANTS(PushConstants, g_PC);

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
    float4 orm = g_Textures[NonUniformIndex(g_PC.OrmTexture)].Sample(
        g_Samplers[NonUniformIndex(g_PC.Sampler)], input.v_UV);
    float channel = g_PC.Channel == 0 ? orm.r : (g_PC.Channel == 1 ? orm.g : orm.b);
    return float4(channel, channel, channel, 1.0);
}
