#include "Veng/surface.slang"

// Deferred opaque fixture fragment stage (gpu) shading a tangent-space normal map on a +Y-facing
// plane: the brick fixture's g-buffer writes, with the normal taken from NormalMap repeated
// UVScale times across the plane, and the roughness widened by the variance the map's mips average
// (SampleNormalVariance), as a lit material does. The map's x tilts toward world X and its y toward
// world Z.
struct MaterialParams
{
    uint  BaseColor;       uint BaseColorSampler;
    uint  NormalMap;       uint NormalMapSampler;
    float MetallicFactor;  float RoughnessFactor;
    float UVScale;         // repeats of the normal map across the plane, per axis
    float NormalStrength;  // scales the map's tangent-space deflection
};

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
    float3 v_WorldNormal : NORMAL;
    nointerpolation uint v_MaterialOffset : TEXCOORD1;
    float4 v_CurClip : TEXCOORD2;
    float4 v_PrevClip : TEXCOORD3;
};

[shader("fragment")]
GBufferOutput fsMain(VSOutput input)
{
    MaterialParams p = g_MaterialParams.Load<MaterialParams>(input.v_MaterialOffset);
    const ViewConstants vc = LoadViewConstants(g_PC.ViewConstantsIndex);

    float4 baseColor = g_Textures[NonUniformResourceIndex(p.BaseColor)].Sample(
        g_Samplers[NonUniformResourceIndex(p.BaseColorSampler)], input.v_UV);

    const float2 uv = input.v_UV * p.UVScale;
    SamplerState normalSampler = g_Samplers[NonUniformResourceIndex(p.NormalMapSampler)];
    float3 tangentNormal =
        UnpackNormal(g_Textures[NonUniformResourceIndex(p.NormalMap)].Sample(normalSampler, uv));
    tangentNormal.xy *= p.NormalStrength;
    const float3 normal = normalize(float3(tangentNormal.x, tangentNormal.z, tangentNormal.y));

    const float variance = SampleNormalVariance(p.NormalMap, normalSampler, uv) *
                           p.NormalStrength * p.NormalStrength;
    const float roughness = WidenRoughnessForNormalMap(vc, p.RoughnessFactor, variance);

    GBufferOutput output;
    output.Albedo = baseColor;
    output.Normal = float4(normal, 0.0);
    output.ORM = float4(1.0, roughness, p.MetallicFactor, 0.0);
    output.Velocity = ComputeMotionVector(input.v_CurClip, input.v_PrevClip);
    output.Emissive = float3(0.0);
    return output;
}
