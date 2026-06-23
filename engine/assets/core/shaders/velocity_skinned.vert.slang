#include "material.slang"

// Skinned TAA velocity prepass vertex stage. The skinned counterpart of velocity.vert: it
// skins the current position with the current palette (DrawData.PaletteBase) and the previous
// position with the previous frame's palette (DrawData.PrevPaletteBase), so the per-object
// motion vector captures both rigid (World/PrevWorld) and deformation (animation) movement.
//
// The palette is one ring-buffered buffer; PaletteBase / PrevPaletteBase are absolute offsets
// into this frame's and the previous frame's regions respectively, both still resident.
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
    float4 v_CurClip : TEXCOORD0;  // unjittered current clip position
    float4 v_PrevClip : TEXCOORD1; // unjittered previous clip position
};

[[vk::binding(0, 2)]] StructuredBuffer<float4x4> g_Palette;

float4x4 BlendPalette(uint base, uint4 indices, float4 weights)
{
    return weights.x * g_Palette[base + indices.x] + weights.y * g_Palette[base + indices.y] +
           weights.z * g_Palette[base + indices.z] + weights.w * g_Palette[base + indices.w];
}

[shader("vertex")]
VSOutput vsMain(VSInput input)
{
    DrawData draw = LoadDrawData(input.a_CandidateId);
    ViewConstants view = LoadViewConstants(g_PC.ViewConstantsIndex);

    float4x4 skinCur = BlendPalette(draw.PaletteBase, input.a_BoneIndices, input.a_BoneWeights);
    float4x4 skinPrev =
        BlendPalette(draw.PrevPaletteBase, input.a_BoneIndices, input.a_BoneWeights);

    float4 curWorld = mul(draw.World, mul(skinCur, float4(input.a_Position, 1.0)));
    float4 prevWorld = mul(draw.PrevWorld, mul(skinPrev, float4(input.a_Position, 1.0)));

    VSOutput output;
    output.sv_position = mul(view.Proj, mul(view.View, curWorld));
    output.v_CurClip = mul(view.CurViewProj, curWorld);
    output.v_PrevClip = mul(view.PrevViewProj, prevWorld);
    return output;
}
