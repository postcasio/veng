// Material fragment fixture whose MaterialParams exceeds MaterialParamStride
// (256 bytes): 17 float4 members = 272 bytes. Used by the material-cook test to
// assert a located over-stride cook error. All members are vectors so the
// struct reflects cleanly — the failure is the stride bound, not field support.

struct MaterialData
{
    uint Albedo;
    uint AlbedoSampler;
    uint Pad0;
    uint Pad1;
};

struct MaterialParams
{
    float4 P0;  float4 P1;  float4 P2;  float4 P3;
    float4 P4;  float4 P5;  float4 P6;  float4 P7;
    float4 P8;  float4 P9;  float4 P10; float4 P11;
    float4 P12; float4 P13; float4 P14; float4 P15;
    float4 P16;
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
    output.Albedo = albedo * p.P0;
    output.Normal = float4(normalize(input.v_WorldNormal), 0.0);
    return output;
}
