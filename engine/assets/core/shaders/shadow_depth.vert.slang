#include "Veng/depth_caster.slang"

// Depth-only shadow vertex stage. Renders the scene's opaque static meshes from the light-space
// view-projection into a shadow map tile; no fragment stage and no color output, only depth.
// VSInput matches veng's canonical vertex layout (Mesh::CanonicalLayout) on binding 0 so it draws
// the same vertex buffers as the geometry pass, plus the instance-rate caster-record index on
// binding 1 (locations follow the canonical attributes). The push block carries only the view's
// light-space view-projection, so it changes per shadow view, not per draw.
struct VSInput
{
    float3 a_Position : POSITION;
    float3 a_Normal : NORMAL;
    float4 a_Tangent : TANGENT;
    float2 a_UV : TEXCOORD0;
    uint   a_CandidateId : TEXCOORD1;  // instance-rate, binding 1: the caster record to place by
};

struct VSOutput
{
    float4 sv_position : SV_Position;
};

struct ShadowPushConstants
{
    float4x4 ViewProj;  // world -> light clip
};

[[vk::push_constant]] ConstantBuffer<ShadowPushConstants> g_PC;

[shader("vertex")]
VSOutput vsMain(VSInput input)
{
    float4 worldPos = mul(g_Casters[input.a_CandidateId].World, float4(input.a_Position, 1.0));

    VSOutput output;
    output.sv_position = mul(g_PC.ViewProj, worldPos);
    return output;
}
