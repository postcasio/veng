// Camera depth + world-normal prepass vertex stage (skinned layout). The skinned counterpart of
// depth_normal_prepass.vert: it linear-blend-skins the position and the normal through the
// per-instance palette before the camera MVP and the world normal matrix, so a skinned mesh's
// posed silhouette and posed surface orientation reach the one-MRT fragment. VSInput matches the
// skinned vertex layout (Mesh::SkinnedLayout).
//
// The palette is bound on set 3 (sets 0-2 are the reserved bindless registry slots), matching the
// renderer's palette descriptor set; the push block adds PaletteBase, the base offset of this
// instance's bone matrices within the palette.
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
    float3 v_WorldNormal : TEXCOORD0;
};

// The world normal matrix is carried as three column vectors rather than a float4x4: with the
// added PaletteBase that keeps the push block within the 128-byte range.
struct DepthNormalSkinnedPushConstants
{
    float4x4 MVP;         // camera world -> clip
    float4 NormalColumn0; // world normal matrix, column 0 (xyz)
    float4 NormalColumn1; // column 1
    float4 NormalColumn2; // column 2
    uint    PaletteBase;  // base offset of this instance's bone matrices in g_Palette
};

[[vk::binding(0, 3)]] StructuredBuffer<float4x4> g_Palette;
[[vk::push_constant]] ConstantBuffer<DepthNormalSkinnedPushConstants> g_PC;

[shader("vertex")]
VSOutput vsMain(VSInput input)
{
    uint base = g_PC.PaletteBase;
    float4x4 skin = input.a_BoneWeights.x * g_Palette[base + input.a_BoneIndices.x] +
                    input.a_BoneWeights.y * g_Palette[base + input.a_BoneIndices.y] +
                    input.a_BoneWeights.z * g_Palette[base + input.a_BoneIndices.z] +
                    input.a_BoneWeights.w * g_Palette[base + input.a_BoneIndices.w];

    float4 skinnedPos = mul(skin, float4(input.a_Position, 1.0));
    float3 skinnedNormal = mul((float3x3)skin, input.a_Normal);

    VSOutput output;
    output.sv_position = mul(g_PC.MVP, skinnedPos);
    output.v_WorldNormal = g_PC.NormalColumn0.xyz * skinnedNormal.x +
                           g_PC.NormalColumn1.xyz * skinnedNormal.y +
                           g_PC.NormalColumn2.xyz * skinnedNormal.z;
    return output;
}
