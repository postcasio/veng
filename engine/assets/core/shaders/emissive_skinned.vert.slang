#include "Veng/material.slang"

// Skinned emissive forward vertex stage. Skins the position through the per-instance bone
// palette like surface_skinned.vert, then forwards only the UV and this draw's emissive
// color + texture handles the emissive fragment needs — not the full surface interpolant set.
struct VSInput
{
    float3 a_Position : POSITION;
    float3 a_Normal : NORMAL;
    float4 a_Tangent : TANGENT;
    float2 a_UV : TEXCOORD0;
    uint4  a_BoneIndices : TEXCOORD1;
    float4 a_BoneWeights : TEXCOORD2;
    uint   a_CandidateId : TEXCOORD3;  // instance-rate, binding 1; fetched at firstInstance
};

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
    nointerpolation float4 v_EmissiveColor : TEXCOORD1;
    nointerpolation uint v_EmissiveTexture : TEXCOORD2;
    nointerpolation uint v_EmissiveSampler : TEXCOORD3;
};

[[vk::binding(0, 2)]] StructuredBuffer<float4x4> g_Palette;

[shader("vertex")]
VSOutput vsMain(VSInput input)
{
    DrawData draw = LoadDrawData(input.a_CandidateId);
    ViewConstants view = LoadViewConstants(g_PC.ViewConstantsIndex);

    uint base = draw.PaletteBase;
    float4x4 skin = input.a_BoneWeights.x * g_Palette[base + input.a_BoneIndices.x] +
                    input.a_BoneWeights.y * g_Palette[base + input.a_BoneIndices.y] +
                    input.a_BoneWeights.z * g_Palette[base + input.a_BoneIndices.z] +
                    input.a_BoneWeights.w * g_Palette[base + input.a_BoneIndices.w];

    float4 skinnedPos = mul(skin, float4(input.a_Position, 1.0));
    float4 worldPos = mul(draw.World, skinnedPos);

    VSOutput output;
    output.sv_position = mul(view.Proj, mul(view.View, worldPos));
    output.v_UV = input.a_UV;
    output.v_EmissiveColor = draw.EmissiveColor;
    output.v_EmissiveTexture = draw.EmissiveTexture;
    output.v_EmissiveSampler = draw.EmissiveSampler;
    return output;
}
