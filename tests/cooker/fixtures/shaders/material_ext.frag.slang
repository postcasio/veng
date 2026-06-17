// Material fragment fixture with an authored block beyond the brick's single
// Factors: MaterialParams { float4 Factors; float Roughness; }. Used by the
// material-cook test to assert the cooked authored block carries Roughness at
// its reflected offset and ParamBytes spans the larger struct.

struct MaterialData
{
    uint Albedo;
    uint AlbedoSampler;
    uint Pad0;
    uint Pad1;
};

struct MaterialParams
{
    float4 Factors;
    float Roughness;
};

static const uint MaterialParamStride = 256;

[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];
[[vk::binding(3, 0)]] StructuredBuffer<MaterialData> g_Materials;
[[vk::binding(4, 0)]] ByteAddressBuffer g_MaterialParams;

MaterialParams LoadMaterialParams(uint index)
{
    return g_MaterialParams.Load<MaterialParams>(index * MaterialParamStride);
}

struct GBufferOutput
{
    float4 Albedo : SV_Target0;
    float4 Normal : SV_Target1;
};

struct PushConstants
{
    float4x4 MVP;
    uint MaterialIndex;
    uint Pad0;
    uint Pad1;
    uint Pad2;
    float3x3 NormalMatrix;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
    float3 v_WorldNormal : NORMAL;
};

[shader("fragment")]
GBufferOutput fsMain(VSOutput input)
{
    MaterialData m = g_Materials[g_PC.MaterialIndex];
    MaterialParams p = LoadMaterialParams(g_PC.MaterialIndex);
    float4 albedo = g_Textures[NonUniformResourceIndex(m.Albedo)].Sample(
        g_Samplers[NonUniformResourceIndex(m.AlbedoSampler)], input.v_UV);

    GBufferOutput output;
    output.Albedo = albedo * p.Factors * p.Roughness;
    output.Normal = float4(normalize(input.v_WorldNormal), 0.0);
    return output;
}
