// Samples a bindless-registered texture/sampler pair selected by push-constant
// indices. The set-0 bindless arrays mirror the BindlessRegistry layout
// (sampled images = 0, samplers = 1) and are excluded from reflection.
#include "Veng/bindless.slang"

struct PushConstants
{
    uint TextureIndex;
    uint SamplerIndex;
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
    return g_Textures[NonUniformIndex(g_PC.TextureIndex)].Sample(
        g_Samplers[NonUniformIndex(g_PC.SamplerIndex)], input.v_UV);
}
