#include "material_data.slang"

// Deferred opaque material fragment stage: reads the per-draw MaterialData entry
// from the bindless set-0 array, samples its albedo texture/sampler by handle
// tinted by Factors, and writes the g-buffer — albedo into G0 (sRGB target),
// the world-space normal into G1. The bindless/MaterialData/texture path is
// unchanged; only the outputs move from a single color target to the MRT.
struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
    float3 v_WorldNormal : NORMAL;
};

[shader("fragment")]
GBufferOutput fsMain(VSOutput input)
{
    MaterialData m = g_Materials[g_PC.MaterialIndex];
    float4 albedo = g_Textures[NonUniformResourceIndex(m.Albedo)].Sample(
        g_Samplers[NonUniformResourceIndex(m.AlbedoSampler)], input.v_UV);

    GBufferOutput output;
    output.Albedo = albedo * m.Factors;
    output.Normal = float4(normalize(input.v_WorldNormal), 0.0);
    return output;
}
