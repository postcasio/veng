#include "Veng/surface_vertex.slang"

// Deferred skinned-surface vertex stage. Identical to surface.vert but for linear-blend
// skinning: VSInput carries the skinned vertex layout (canonical attributes plus four bone
// indices and weights) and the stage blends four bone matrices from the per-instance
// skinning palette, then hands the deformed object-space vertex to the shared transform in
// Veng/surface_vertex.slang — the same one the static stage uses, so both write the identical
// SurfaceFragmentInput.
//
// The palette lives on its own descriptor set (set 2) so the static surface set-1 (DrawData)
// layout is unchanged; the renderer fills it per skinned instance and DrawData.PaletteBase is
// the base offset of this instance's bone matrices within it.
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

// The per-instance skinning palette: each bone's GlobalInverse * modelBone * InverseBind
// matrix, indexed by DrawData.PaletteBase + boneIndex. Vertex-stage only, off the static
// set-1 layout (a dedicated set 2 the renderer binds for skinned draws).
[[vk::binding(0, 2)]] StructuredBuffer<float4x4> g_Palette;

[shader("vertex")]
SurfaceFragmentInput vsMain(VSInput input)
{
    DrawData draw = LoadDrawData(input.a_CandidateId);
    ViewConstants view = LoadViewConstants(g_PC.ViewConstantsIndex);

    // Linear-blend skinning: the weighted sum of the four influencing bone matrices.
    uint base = draw.PaletteBase;
    float4x4 skin = input.a_BoneWeights.x * g_Palette[base + input.a_BoneIndices.x] +
                    input.a_BoneWeights.y * g_Palette[base + input.a_BoneIndices.y] +
                    input.a_BoneWeights.z * g_Palette[base + input.a_BoneIndices.z] +
                    input.a_BoneWeights.w * g_Palette[base + input.a_BoneIndices.w];

    float4 skinnedPos = mul(skin, float4(input.a_Position, 1.0));
    float3 skinnedNormal = mul((float3x3)skin, input.a_Normal);
    float3 skinnedTangent = mul((float3x3)skin, input.a_Tangent.xyz);

    // Previous frame's deformation: the same vertex skinned by the previous-frame palette
    // (still resident in the ring-buffered palette), so the motion vector captures both
    // rigid (World/PrevWorld) and deformation (animation) movement.
    uint prevBase = draw.PrevPaletteBase;
    float4x4 skinPrev =
        input.a_BoneWeights.x * g_Palette[prevBase + input.a_BoneIndices.x] +
        input.a_BoneWeights.y * g_Palette[prevBase + input.a_BoneIndices.y] +
        input.a_BoneWeights.z * g_Palette[prevBase + input.a_BoneIndices.z] +
        input.a_BoneWeights.w * g_Palette[prevBase + input.a_BoneIndices.w];
    float4 prevSkinnedPos = mul(skinPrev, float4(input.a_Position, 1.0));

    return AssembleSurfaceVertex(draw, view, skinnedPos, skinnedNormal,
                                 float4(skinnedTangent, input.a_Tangent.w), input.a_UV,
                                 prevSkinnedPos);
}
