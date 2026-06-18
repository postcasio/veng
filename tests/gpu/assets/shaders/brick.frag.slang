#include "material_data.slang"

// Deferred opaque material fragment stage: loads the per-draw MaterialParams
// block from the bindless set-0 buffer, samples its albedo texture/sampler by
// handle tinted by Factors, and writes the g-buffer — albedo into G0 (sRGB
// target), the world-space normal into G1. The handle slots and the Factors tint
// share one block; the bindless/texture path is unchanged.
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
    return output;
}
