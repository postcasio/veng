// AO-visualize fragment stage: samples the screen-space ambient-occlusion target
// (the SsaoScenePass's single-channel R8 result) through the bindless set-0 array
// and writes it as greyscale — 1 is unoccluded (white), 0 fully occluded (black).
// A debug view of the SSAO term before the lighting pass folds it into the ambient.
#include "Veng/bindless.slang"

struct PushConstants
{
    uint AoTexture;
    uint Sampler;
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
    float ao = g_Textures[NonUniformIndex(g_PC.AoTexture)].Sample(
        g_Samplers[NonUniformIndex(g_PC.Sampler)], input.v_UV).r;
    return float4(ao, ao, ao, 1.0);
}
