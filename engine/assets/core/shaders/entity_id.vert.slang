#include "Veng/material.slang"

// Entity-id picking vertex stage (static meshes). A minimal sibling of surface.vert: it reads
// the per-draw DrawData by the instance-rate candidate id and emits only the clip position and
// the entity slot index — the picking fragment writes index + 1 into the EntityId target. It
// shares the surface pipeline layout (set 0 bindless + set 1 DrawData + the SurfacePush) and the
// canonical vertex layout, so the picking pipeline drives the same draws as the g-buffer pass.
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
    nointerpolation uint v_EntityIndex : TEXCOORD0;  // packed Entity slot index for the fragment
};

[shader("vertex")]
VSOutput vsMain(VSInput input)
{
    DrawData draw = LoadDrawData(input.a_CandidateId);
    ViewConstants view = LoadViewConstants(g_PC.ViewConstantsIndex);

    float4 worldPos = mul(draw.World, float4(input.a_Position, 1.0));

    VSOutput output;
    output.sv_position = mul(view.Proj, mul(view.View, worldPos));
    output.v_EntityIndex = draw.EntityIndex;
    return output;
}
