#include "Veng/material.slang"

// Emissive forward fragment stage. Outputs only the additive emissive term — the
// per-draw emissive color modulated by an optional emissive texture — into the lit HDR
// target with additive blending. Emissive is lighting-independent, so this is a pure
// output addend (no albedo, no BRDF): a dark-albedo surface glows and a shadowed emitter
// stays lit. A draw whose emissive color is zero contributes nothing under additive blend.
struct FSInput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
    nointerpolation float4 v_EmissiveColor : TEXCOORD1;
    nointerpolation uint v_EmissiveTexture : TEXCOORD2;
    nointerpolation uint v_EmissiveSampler : TEXCOORD3;
};

[shader("fragment")]
float4 fsMain(FSInput input) : SV_Target0
{
    float3 emissive = input.v_EmissiveColor.rgb;
    if (input.v_EmissiveTexture != 0)
    {
        emissive *= g_Textures[NonUniformResourceIndex(input.v_EmissiveTexture)].Sample(
            g_Samplers[NonUniformResourceIndex(input.v_EmissiveSampler)], input.v_UV).rgb;
    }
    return float4(emissive, 1.0);
}
