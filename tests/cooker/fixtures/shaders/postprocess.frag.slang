// PostProcess fragment fixture: writes a single float4 SV_Target0 (the
// PostProcess domain's output contract). Used by the material-cook test to assert
// a postprocess material cooks and a surface material against this shader fails.

struct MaterialParams
{
    uint  Hdr;
    uint  HdrSampler;
    float Exposure;
};

static const uint MaterialParamStride = 256;

[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];
[[vk::binding(4, 0)]] ByteAddressBuffer g_MaterialParams;

MaterialParams LoadMaterialParams(uint index)
{
    return g_MaterialParams.Load<MaterialParams>(index * MaterialParamStride);
}

struct PushConstants
{
    float4x4 MVP;
    uint MaterialIndex;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target0
{
    MaterialParams p = LoadMaterialParams(g_PC.MaterialIndex);
    float4 color = g_Textures[NonUniformResourceIndex(p.Hdr)].Sample(
        g_Samplers[NonUniformResourceIndex(p.HdrSampler)], input.v_UV);
    return color * p.Exposure;
}
