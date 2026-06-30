#include "Veng/material.slang"

// Deferred opaque character material fragment stage for the RenderPeople scan: a
// diffuse + normal + glossiness set rather than a packed ORM map. It samples base color
// (tinted by BaseColorFactor) and a glossiness map (roughness = 1 - gloss), treats the
// surface as a dielectric (metallic from the factor, occlusion from the factor), perturbs
// the world normal by the tangent-space normal map, and writes the g-buffer. The ORM
// material slot carries the glossiness texture; the Normal slot the tangent-space normal.

// This material's parameter block: the bindless handle slots beside the authored
// scalar/vector uniforms, laid out by reflection. The ORM slot carries the glossiness
// map. Read through the engine's set-0 g_MaterialParams at index * MaterialParamStride.
struct MaterialParams
{
    uint   BaseColor;        uint BaseColorSampler;   // base-color texture handle + sampler
    uint   ORM;              uint ORMSampler;         // glossiness texture handle + sampler
    uint   Normal;           uint NormalSampler;      // tangent-space normal-map handle + sampler (0 = unbound)
    uint   Pad0;             uint Pad1;
    float4 BaseColorFactor;  // tint applied to the sampled base color
    float4 EmissiveFactor;   // rgb reserved (colored emissive is a follow-on); a = strength
    float  MetallicFactor;   float RoughnessFactor;
    float  OcclusionStrength; float Pad2;
};

MaterialParams LoadMaterialParams(uint index)
{
    return g_MaterialParams.Load<MaterialParams>(index * MaterialParamStride);
}

[shader("fragment")]
GBufferOutput fsMain(SurfaceFragmentInput input)
{
    MaterialParams p = LoadMaterialParams(input.v_MaterialIndex);

    float4 baseColor = g_Textures[NonUniformResourceIndex(p.BaseColor)].Sample(
        g_Samplers[NonUniformResourceIndex(p.BaseColorSampler)], input.v_UV);

    // The ORM slot carries the glossiness map; roughness is its complement, scaled by the
    // authored RoughnessFactor. A scanned human is a dielectric — metallic stays at its
    // (zero) factor and occlusion at the OcclusionStrength factor.
    float gloss = g_Textures[NonUniformResourceIndex(p.ORM)].Sample(
        g_Samplers[NonUniformResourceIndex(p.ORMSampler)], input.v_UV).r;
    float roughness = saturate((1.0 - gloss) * p.RoughnessFactor);
    float metallic = p.MetallicFactor;
    float occlusion = p.OcclusionStrength;

    float3 worldNormal = normalize(input.v_WorldNormal);
    if (p.Normal != 0)
    {
        float4 sampled = g_Textures[NonUniformResourceIndex(p.Normal)].Sample(
            g_Samplers[NonUniformResourceIndex(p.NormalSampler)], input.v_UV);
        float3 tangentNormal = UnpackNormal(sampled);

        float3 T = normalize(input.v_WorldTangent.xyz -
                             worldNormal * dot(worldNormal, input.v_WorldTangent.xyz));
        float3 B = cross(worldNormal, T) * input.v_WorldTangent.w;
        float3x3 TBN = float3x3(T, B, worldNormal);

        worldNormal = normalize(mul(tangentNormal, TBN));
    }

    GBufferOutput output;
    output.Albedo = baseColor * p.BaseColorFactor;
    output.Normal = float4(worldNormal, 0.0);
    output.ORM = float4(occlusion, roughness, metallic, p.EmissiveFactor.a);
    output.Velocity = ComputeMotionVector(input.v_CurClip, input.v_PrevClip);
    return output;
}
