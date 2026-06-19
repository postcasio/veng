#include "material_data.slang"

// Deferred opaque material fragment stage (cooker fixture): loads the per-draw
// MaterialParams block, samples its albedo texture/sampler by handle tinted by
// Factors, and writes the three-target g-buffer — albedo into G0, the world-space
// normal into G1, and a constant packed ORM into G2 (the surface output contract).
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
    output.Albedo = albedo * p.Factors;
    output.Normal = float4(normalize(input.v_WorldNormal), 0.0);
    output.ORM = float4(1.0, 1.0, 0.0, 0.0);
    return output;
}
