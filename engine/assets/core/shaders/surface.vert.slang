#include "Veng/material.slang"

// Deferred surface vertex stage. VSInput matches veng's canonical vertex layout
// (Mesh::CanonicalLayout / the core pack's canonical layout):
// position/normal/tangent[float4]/uv on binding 0 — the layout cooked surface meshes
// are uploaded in — plus an instance-rate candidate id on binding 1 (locations follow
// the canonical attributes). The candidate id indexes the per-draw DrawData SSBO for
// this draw's world matrix, normal matrix, and material selector; the view-projection
// comes from the per-frame set-0 view constants.
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
    float3 v_WorldNormal : NORMAL;
    float4 v_WorldTangent : TANGENT;  // xyz world tangent; w handedness sign for the bitangent
    nointerpolation uint v_MaterialIndex : TEXCOORD1;  // per-draw material selector for the fragment
    float4 v_CurClip : TEXCOORD2;   // unjittered current clip position (g-buffer motion vector)
    float4 v_PrevClip : TEXCOORD3;  // unjittered previous clip position (g-buffer motion vector)
};

[shader("vertex")]
VSOutput vsMain(VSInput input)
{
    DrawData draw = LoadDrawData(input.a_CandidateId);
    ViewConstants view = LoadViewConstants(g_PC.ViewConstantsIndex);

    // The normal matrix (inverse-transpose of World's upper 3x3) is carried as its
    // three columns; transforming a vector is the column-weighted sum, matching the
    // column-major glm matrix the renderer uploads.
    float3 c0 = draw.NormalColumn0.xyz;
    float3 c1 = draw.NormalColumn1.xyz;
    float3 c2 = draw.NormalColumn2.xyz;

    float4 worldPos = mul(draw.World, float4(input.a_Position, 1.0));
    float4 prevWorldPos = mul(draw.PrevWorld, float4(input.a_Position, 1.0));

    VSOutput output;
    output.sv_position = mul(view.Proj, mul(view.View, worldPos));
    // The motion vector is measured in unjittered clip space so the jitter the TAA
    // resolve is integrating does not leak into the reprojection.
    output.v_CurClip = mul(view.CurViewProj, worldPos);
    output.v_PrevClip = mul(view.PrevViewProj, prevWorldPos);
    output.v_UV = input.a_UV;
    output.v_WorldNormal = c0 * input.a_Normal.x + c1 * input.a_Normal.y + c2 * input.a_Normal.z;
    // NormalMatrix carries the tangent to world space alongside the normal; the
    // canonical tangent's w is the bitangent handedness sign, passed through.
    output.v_WorldTangent =
        float4(c0 * input.a_Tangent.x + c1 * input.a_Tangent.y + c2 * input.a_Tangent.z,
               input.a_Tangent.w);
    output.v_MaterialIndex = draw.MaterialIndex;
    return output;
}
