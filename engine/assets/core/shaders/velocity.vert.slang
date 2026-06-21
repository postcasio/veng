#include "material.slang"

// TAA velocity prepass vertex stage. Renders the same canonical-layout geometry as
// the g-buffer pass (same per-draw DrawData, same instance-rate candidate id), but
// carries both the current and previous screen positions so the fragment can write a
// per-object motion vector. sv_position uses the same jittered view-projection as the
// g-buffer pass, so the velocity pass depth-tests against the populated g-buffer depth
// and only the visible surface writes its velocity.
//
// The motion is measured in unjittered clip space (CurViewProj / PrevViewProj), so the
// jitter — which TAA is integrating — does not leak into the reprojection.
struct VSInput
{
    float3 a_Position : POSITION;
    float3 a_Normal : NORMAL;
    float4 a_Tangent : TANGENT;
    float2 a_UV : TEXCOORD0;
    uint   a_CandidateId : TEXCOORD1; // instance-rate, binding 1; fetched at firstInstance
};

struct VSOutput
{
    float4 sv_position : SV_Position;
    float4 v_CurClip : TEXCOORD0;  // unjittered current clip position
    float4 v_PrevClip : TEXCOORD1; // unjittered previous clip position
};

[shader("vertex")]
VSOutput vsMain(VSInput input)
{
    DrawData draw = LoadDrawData(input.a_CandidateId);
    ViewConstants view = LoadViewConstants(g_PC.ViewConstantsIndex);

    float4 worldPos = mul(draw.World, float4(input.a_Position, 1.0));
    float4 prevWorldPos = mul(draw.PrevWorld, float4(input.a_Position, 1.0));

    VSOutput output;
    // The jittered projection matches the g-buffer pass exactly, so the depth test
    // selects the same front surface.
    output.sv_position = mul(view.Proj, mul(view.View, worldPos));
    output.v_CurClip = mul(view.CurViewProj, worldPos);
    output.v_PrevClip = mul(view.PrevViewProj, prevWorldPos);
    return output;
}
