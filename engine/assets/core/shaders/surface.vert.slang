#include "Veng/surface_vertex.slang"

// Deferred surface vertex stage. VSInput matches veng's canonical vertex layout
// (Mesh::CanonicalLayout / the core pack's canonical layout):
// position/normal/tangent[float4]/uv on binding 0 — the layout cooked surface meshes
// are uploaded in — plus an instance-rate candidate id on binding 1 (locations follow
// the canonical attributes). The candidate id indexes the per-draw DrawData SSBO for
// this draw's world matrix, normal matrix, and material selector; the view-projection
// comes from the per-frame set-0 view constants. The output struct (SurfaceFragmentInput)
// and the world/normal transform live in Veng/surface_vertex.slang, shared with the skinned
// stage.
struct VSInput
{
    float3 a_Position : POSITION;
    float3 a_Normal : NORMAL;
    float4 a_Tangent : TANGENT;
    float2 a_UV : TEXCOORD0;
    uint   a_CandidateId : TEXCOORD1;  // instance-rate, binding 1; fetched at firstInstance
};

[shader("vertex")]
SurfaceFragmentInput vsMain(VSInput input)
{
    DrawData draw = LoadDrawData(input.a_CandidateId);
    ViewConstants view = LoadViewConstants(g_PC.ViewConstantsIndex);

    // A static mesh's object-space vertex is the canonical attribute set unchanged, and its
    // previous object position equals its current — only World/PrevWorld move it.
    float4 objPos = float4(input.a_Position, 1.0);
    return AssembleSurfaceVertex(draw, input.a_CandidateId, view, objPos, input.a_Normal,
                                 input.a_Tangent, input.a_UV, objPos);
}
