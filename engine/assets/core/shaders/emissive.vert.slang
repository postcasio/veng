#include "Veng/material.slang"

// Emissive forward vertex stage. Shares the canonical vertex layout with surface.vert
// (position/normal/tangent/uv on binding 0, the instance-rate candidate id on binding
// 1) so the emissive pass re-rasterizes the same gathered geometry. It carries only the
// data the emissive fragment needs — the UV and this draw's emissive color + texture
// handles, read from the per-draw DrawData SSBO — not the full surface interpolant set.
struct VSInput
{
    float3 a_Position : POSITION;
    float3 a_Normal : NORMAL;
    float4 a_Tangent : TANGENT;
    float2 a_UV : TEXCOORD0;
    uint   a_CandidateId : TEXCOORD1;  // instance-rate, binding 1; fetched at firstInstance
};

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
    nointerpolation float4 v_EmissiveColor : TEXCOORD1;
    nointerpolation uint v_EmissiveTexture : TEXCOORD2;
    nointerpolation uint v_EmissiveSampler : TEXCOORD3;
};

[shader("vertex")]
VSOutput vsMain(VSInput input)
{
    DrawData draw = LoadDrawData(input.a_CandidateId);
    ViewConstants view = LoadViewConstants(g_PC.ViewConstantsIndex);

    float4 worldPos = mul(draw.World, float4(input.a_Position, 1.0));

    VSOutput output;
    output.sv_position = mul(view.Proj, mul(view.View, worldPos));
    output.v_UV = input.a_UV;
    output.v_EmissiveColor = draw.EmissiveColor;
    output.v_EmissiveTexture = draw.EmissiveTexture;
    output.v_EmissiveSampler = draw.EmissiveSampler;
    return output;
}
