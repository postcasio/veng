#include "material_data.slang"

// Forward material fragment stage (planset-5 plan 09 fixture): reads the
// per-draw MaterialData entry from the bindless set-0 array and samples its
// albedo texture/sampler by handle, tinted by Factors.
struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
    MaterialData m = g_Materials[g_PC.MaterialIndex];
    float4 albedo = g_Textures[NonUniformResourceIndex(m.Albedo)].Sample(
        g_Samplers[NonUniformResourceIndex(m.AlbedoSampler)], input.v_UV);
    return albedo * m.Factors;
}
