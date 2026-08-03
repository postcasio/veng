#include "Veng/surface.slang"

// Full surface vertex stage (gpu fixture): writes the complete SurfaceFragmentInput interpolant
// set (UV, world normal, world tangent, material selector, current/previous clip, candidate id)
// the engine's Veng/surface.slang declares, so a fragment taking SurfaceFragmentInput links
// against it. The
// trimmed brick.vert fixture omits the tangent; the translucent fixture material uses this full
// stage instead so its Veng/translucent.slang fragment (which re-exports SurfaceFragmentInput)
// matches the vertex output exactly.
struct VSInput
{
    float3 a_Position : POSITION;
    float3 a_Normal : NORMAL;
    float4 a_Tangent : TANGENT;
    float2 a_UV : TEXCOORD0;
    uint   a_CandidateId : TEXCOORD1; // instance-rate, binding 1; fetched at firstInstance
};

[shader("vertex")]
SurfaceFragmentInput vsMain(VSInput input)
{
    DrawData draw = LoadDrawData(input.a_CandidateId);
    ViewConstants view = LoadViewConstants(g_PC.ViewConstantsIndex);

    float3 c0 = draw.NormalColumn0.xyz;
    float3 c1 = draw.NormalColumn1.xyz;
    float3 c2 = draw.NormalColumn2.xyz;

    float4 worldPos = mul(draw.World, float4(input.a_Position, 1.0));
    float4 prevWorldPos = mul(draw.PrevWorld, float4(input.a_Position, 1.0));

    SurfaceFragmentInput output;
    output.sv_position = mul(view.Proj, mul(view.View, worldPos));
    output.v_CurClip = mul(view.CurViewProj, worldPos);
    output.v_PrevClip = mul(view.PrevViewProj, prevWorldPos);
    output.v_UV = input.a_UV;
    output.v_WorldNormal = c0 * input.a_Normal.x + c1 * input.a_Normal.y + c2 * input.a_Normal.z;
    output.v_WorldTangent =
        float4(c0 * input.a_Tangent.x + c1 * input.a_Tangent.y + c2 * input.a_Tangent.z,
               input.a_Tangent.w);
    output.v_MaterialIndex = draw.MaterialIndex;
    output.v_CandidateId = input.a_CandidateId;
    return output;
}
