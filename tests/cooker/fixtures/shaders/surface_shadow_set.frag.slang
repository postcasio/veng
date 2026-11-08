// Surface fragment fixture declaring a binding in set 5: the material importer admits a Surface
// fragment's set 3 and its shadow set 5, and refuses set 4 (the skinning palette's, or a sphere
// terrain's tiles) unless the material declares "sphereTerrain".

#include "Veng/bindless.slang"
struct FixtureSet3
{
    ByteAddressBuffer DrawData;
};
VE_DESCRIPTOR_SET(3, FixtureSet3, g_Set3);
#define g_DrawData g_Set3.DrawData
struct FixtureSet5
{
    ByteAddressBuffer Extra;
};
VE_DESCRIPTOR_SET(5, FixtureSet5, g_Set5);
#define g_Extra g_Set5.Extra

struct MaterialParams
{
    float4 Factors;
};

struct GBufferOutput
{
    float4 Albedo : SV_Target0;
    float4 Normal : SV_Target1;
    float4 ORM    : SV_Target2;
    float2 Velocity : SV_Target3;
    float3 Emissive : SV_Target4;
};

struct VSOutput
{
    float4 sv_position : SV_Position;
    float3 v_WorldNormal : NORMAL;
};

[shader("fragment")]
GBufferOutput fsMain(VSOutput input)
{
    const MaterialParams p = g_MaterialParams.Load<MaterialParams>(g_DrawData.Load(0));
    GBufferOutput output;
    output.Albedo = p.Factors * asfloat(g_Extra.Load(0));
    output.Normal = float4(normalize(input.v_WorldNormal), 0.0);
    output.ORM = float4(1.0, 1.0, 0.0, 0.0);
    output.Velocity = float2(0.0, 0.0);
    output.Emissive = float3(0.0);
    return output;
}
