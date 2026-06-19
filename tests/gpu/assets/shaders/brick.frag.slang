#include "material_data.slang"

// Deferred opaque metallic-roughness material fragment stage (gpu fixture): loads
// the per-draw MaterialParams block, samples base color tinted by BaseColorFactor,
// and writes the g-buffer — base color into G0, the world-space normal into G1,
// and the authored occlusion/roughness/metallic/emissive factors into G2. The
// deferred-lighting oracle reads back known roughness/metallic values, so this
// fixture writes the factors directly rather than sampling a packed ORM texture.
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

    GBufferOutput output;
    output.Albedo = baseColor * p.BaseColorFactor;
    output.Normal = float4(normalize(input.v_WorldNormal), 0.0);
    output.ORM = float4(1.0, p.RoughnessFactor, p.MetallicFactor, p.EmissiveFactor.a);
    return output;
}
