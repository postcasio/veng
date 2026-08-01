// Material fragment fixture whose MaterialParams exceeds MaterialParamStride
// (1024 bytes): four handle uints + 68 float4 members = 16 + 1088 = 1104 bytes.
// Used by the material-cook test to assert a located over-stride cook error. All
// param members are vectors so the struct reflects cleanly — the failure is the
// stride bound, not field support.

struct MaterialParams
{
    uint Albedo;
    uint AlbedoSampler;
    uint Pad0;
    uint Pad1;
    float4 P0;  float4 P1;  float4 P2;  float4 P3;
    float4 P4;  float4 P5;  float4 P6;  float4 P7;
    float4 P8;  float4 P9;  float4 P10; float4 P11;
    float4 P12; float4 P13; float4 P14; float4 P15;
    float4 P16; float4 P17; float4 P18; float4 P19;
    float4 P20; float4 P21; float4 P22; float4 P23;
    float4 P24; float4 P25; float4 P26; float4 P27;
    float4 P28; float4 P29; float4 P30; float4 P31;
    float4 P32; float4 P33; float4 P34; float4 P35;
    float4 P36; float4 P37; float4 P38; float4 P39;
    float4 P40; float4 P41; float4 P42; float4 P43;
    float4 P44; float4 P45; float4 P46; float4 P47;
    float4 P48; float4 P49; float4 P50; float4 P51;
    float4 P52; float4 P53; float4 P54; float4 P55;
    float4 P56; float4 P57; float4 P58; float4 P59;
    float4 P60; float4 P61; float4 P62; float4 P63;
    float4 P64; float4 P65; float4 P66; float4 P67;
};

static const uint MaterialParamStride = 1024;

[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];
[[vk::binding(4, 0)]] ByteAddressBuffer g_MaterialParams;

MaterialParams LoadMaterialParams(uint index)
{
    return g_MaterialParams.Load<MaterialParams>(index * MaterialParamStride);
}

struct GBufferOutput
{
    float4 Albedo : SV_Target0;
    float4 Normal : SV_Target1;
    float4 ORM    : SV_Target2;
    float2 Velocity : SV_Target3;  // screen-space motion vector (curUV - prevUV)
    float3 Emissive : SV_Target4;  // linear HDR emissive radiance (non-negative)
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
    MaterialParams p = LoadMaterialParams(g_PC.MaterialIndex);
    float4 albedo = g_Textures[NonUniformResourceIndex(p.Albedo)].Sample(
        g_Samplers[NonUniformResourceIndex(p.AlbedoSampler)], input.v_UV);

    GBufferOutput output;
    output.Albedo = albedo * p.P0;
    output.Normal = float4(normalize(input.v_WorldNormal), 0.0);
    output.ORM = float4(1.0, 1.0, 0.0, 0.0);
    output.Velocity = float2(0.0, 0.0);
    output.Emissive = float3(0.0);
    return output;
}
