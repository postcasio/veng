#include "material_data.slang"

// Deferred opaque metallic-roughness material fragment stage: loads the per-draw
// MaterialParams block, samples base color (tinted by BaseColorFactor) and the
// packed ORM texture, applies the metallic/roughness/occlusion factors, and writes
// the g-buffer — base color into G0 (sRGB target), the world-space normal into G1,
// and packed occlusion/roughness/metallic/emissive into G2. The handle slots and
// the authored factors share one block; the bindless/texture path is unchanged.
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

    GBufferOutput output;
    output.Albedo = baseColor * p.BaseColorFactor;
    output.Normal = float4(normalize(input.v_WorldNormal), 0.0);
    output.ORM = float4(occlusion, roughness, metallic, p.EmissiveFactor.a);
    return output;
}
