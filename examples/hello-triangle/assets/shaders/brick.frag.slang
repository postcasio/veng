#include "Veng/material.slang"

// Deferred opaque metallic-roughness material fragment stage: loads the per-draw
// MaterialParams block, samples base color (tinted by BaseColorFactor) and the
// packed ORM texture, applies the metallic/roughness/occlusion factors, perturbs
// the world normal by the tangent-space normal map, and writes the g-buffer — base
// color into G0 (sRGB target), the perturbed world-space normal into G1, and packed
// occlusion/roughness/metallic/emissive into G2. The handle slots and the authored
// factors share one block; the bindless/texture path is unchanged.

// This material's parameter block: the bindless handle slots (uint members the loader
// patches with a resolved index) beside the authored scalar/vector uniforms, laid out
// by reflection. The cooker reflects this struct to validate the .vmat fields and pack
// their values at the reflected offsets, read through the engine's set-0 g_MaterialParams
// at index * MaterialParamStride.
struct MaterialParams
{
    uint   BaseColor;        uint BaseColorSampler;   // base-color texture handle + sampler
    uint   ORM;              uint ORMSampler;         // occlusion-roughness-metallic texture handle + sampler
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
    float4 orm = g_Textures[NonUniformResourceIndex(p.ORM)].Sample(
        g_Samplers[NonUniformResourceIndex(p.ORMSampler)], input.v_UV);

    // ORM texture: R occlusion, G roughness, B metallic. Combine with the
    // authored factors; the lighting pass gates ambient by ORM.r, so the
    // occlusion strength is baked in here.
    float occlusion = lerp(1.0, orm.r, p.OcclusionStrength);
    float roughness = orm.g * p.RoughnessFactor;
    float metallic = orm.b * p.MetallicFactor;

    float3 worldNormal = normalize(input.v_WorldNormal);

    // Tangent-space normal mapping. An unbound normal handle (slot 0) leaves the
    // geometric normal — identity perturbation. Otherwise the TBN is built from the
    // interpolated world normal + Gram-Schmidt-orthogonalized world tangent, with
    // the bitangent's handedness from the canonical tangent's w sign.
    if (p.Normal != 0)
    {
        float3 sampled = g_Textures[NonUniformResourceIndex(p.Normal)].Sample(
            g_Samplers[NonUniformResourceIndex(p.NormalSampler)], input.v_UV).xyz;
        float3 tangentNormal = sampled * 2.0 - 1.0;  // [0,1] → [-1,1]

        float3 T = normalize(input.v_WorldTangent.xyz - worldNormal * dot(worldNormal, input.v_WorldTangent.xyz));
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
