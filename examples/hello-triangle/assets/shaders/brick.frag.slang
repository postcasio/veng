#include "material_data.slang"

// Deferred opaque metallic-roughness material fragment stage: loads the per-draw
// MaterialParams block, samples base color (tinted by BaseColorFactor) and the
// packed ORM texture, applies the metallic/roughness/occlusion factors, perturbs
// the world normal by the tangent-space normal map, and writes the g-buffer — base
// color into G0 (sRGB target), the perturbed world-space normal into G1, and packed
// occlusion/roughness/metallic/emissive into G2. The handle slots and the authored
// factors share one block; the bindless/texture path is unchanged.
struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
    float3 v_WorldNormal : NORMAL;
    float4 v_WorldTangent : TANGENT;  // xyz world tangent; w handedness sign for the bitangent
};

[shader("fragment")]
GBufferOutput fsMain(VSOutput input)
{
    MaterialParams p = LoadMaterialParams(g_PC.MaterialIndex);

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
    return output;
}
