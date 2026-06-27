#include "material.slang"

// Entity-id picking vertex stage (skinned meshes). The skinned sibling of entity_id.vert: it
// linear-blend skins the position through the per-instance palette (set 2) before the world and
// view-projection transforms, and emits the entity slot index for the picking fragment. Shares
// the skinned surface pipeline layout (adds set 2, the palette) and the skinned vertex layout.
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
    nointerpolation uint v_EntityIndex : TEXCOORD0;
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
    output.v_EntityIndex = draw.EntityIndex;
    return output;
}
