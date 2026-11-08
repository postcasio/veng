// ImGui fragment stage. Modulates the vertex colour by the draw command's texture, which the
// push constants address by its bindless texture and sampler slots; set 0 is the bindless
// registry. ImGui's solid fills sample the font atlas's white texel, so every draw samples.

struct PushConstants
{
    float2 Scale;
    float2 Translate;
    uint Texture;
    uint Sampler;
};

#include "Veng/bindless.slang"
VE_PUSH_CONSTANTS(PushConstants, g_PC);

struct VSOutput
{
    float4 sv_position : SV_Position;
    float4 v_Color : COLOR0;
    float2 v_UV : TEXCOORD0;
};

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
    return input.v_Color * g_Textures[g_PC.Texture].Sample(g_Samplers[g_PC.Sampler], input.v_UV);
}
