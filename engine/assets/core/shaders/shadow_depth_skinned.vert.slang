// Depth-only skinned shadow vertex stage. The skinned counterpart of shadow_depth.vert:
// it linear-blend-skins the position through the per-instance palette before the light-space
// MVP, so a skinned mesh casts its posed (animated) shadow. VSInput matches the skinned vertex
// layout (Mesh::SkinnedLayout); no fragment stage and no color output, only depth.
//
// The palette is bound on set 1 (set 0 stays the reserved bindless slot); the push block adds
// PaletteBase, the base offset of this instance's bone matrices within the palette.
struct VSInput
{
    float3 a_Position : POSITION;
    float3 a_Normal : NORMAL;
    float4 a_Tangent : TANGENT;
    float2 a_UV : TEXCOORD0;
    uint4  a_BoneIndices : TEXCOORD1;
    float4 a_BoneWeights : TEXCOORD2;
};

struct VSOutput
{
    float4 sv_position : SV_Position;
};

struct ShadowPushConstants
{
    float4x4 MVP;       // light-space world → clip
    uint     PaletteBase; // base offset of this instance's bone matrices in g_Palette
};

[[vk::binding(0, 1)]] StructuredBuffer<float4x4> g_Palette;
[[vk::push_constant]] ConstantBuffer<ShadowPushConstants> g_PC;

[shader("vertex")]
VSOutput vsMain(VSInput input)
{
    uint base = g_PC.PaletteBase;
    float4x4 skin = input.a_BoneWeights.x * g_Palette[base + input.a_BoneIndices.x] +
                    input.a_BoneWeights.y * g_Palette[base + input.a_BoneIndices.y] +
                    input.a_BoneWeights.z * g_Palette[base + input.a_BoneIndices.z] +
                    input.a_BoneWeights.w * g_Palette[base + input.a_BoneIndices.w];

    float4 skinnedPos = mul(skin, float4(input.a_Position, 1.0));

    VSOutput output;
    output.sv_position = mul(g_PC.MVP, skinnedPos);
    return output;
}
