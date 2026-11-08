// PostProcess fragment fixture: writes a single float4 SV_Target0 (the
// PostProcess domain's output contract). Used by the material-cook test to assert
// a postprocess material cooks and a surface material against this shader fails.

#include "Veng/bindless.slang"

struct MaterialParams
{
    uint  Hdr;
    uint  HdrSampler;
    float Exposure;
};

MaterialParams LoadMaterialParams(uint offset)
{
    return g_MaterialParams.Load<MaterialParams>(offset);
}

struct PushConstants
{
    float4x4 MVP;
    uint MaterialOffset;
};

VE_PUSH_CONSTANTS(PushConstants, g_PC);

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target0
{
    MaterialParams p = LoadMaterialParams(g_PC.MaterialOffset);
    float4 color = g_Textures[NonUniformIndex(p.Hdr)].Sample(
        g_Samplers[NonUniformIndex(p.HdrSampler)], input.v_UV);
    return color * p.Exposure;
}
