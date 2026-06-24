// Material fragment fixture with no handle fields — a params-only material.
// Used by the material-cook test to assert a material can declare zero handle
// fields (the handle count is shader-driven, not a fixed engine struct): the
// field table holds only Kind 0 params and the block carries just the params.

struct MaterialParams
{
    float4 Factors;
    float  Strength;
};

static const uint MaterialParamStride = 256;

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

    GBufferOutput output;
    output.Albedo = p.Factors * p.Strength;
    output.Normal = float4(normalize(input.v_WorldNormal), 0.0);
    output.ORM = float4(1.0, 1.0, 0.0, 0.0);
    output.Velocity = float2(0.0, 0.0);
    return output;
}
