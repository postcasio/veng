#include "Veng/depth_caster.slang"

// Camera depth + world-normal prepass vertex stage (static/canonical layout). Rasterizes
// opaque geometry from the camera view and passes only the clip position and the world-space
// normal — the lean geometry render path's static variant. Like the depth-only shadow caster
// (shadow_depth.vert) it is instanced: the instance-rate caster-record index on binding 1 selects
// the mesh's world matrix and world normal matrix, and the push block carries only the camera's
// view-projection. VSInput matches veng's canonical vertex layout (Mesh::CanonicalLayout) so it
// draws the same vertex buffers as the g-buffer pass; there is no material sample.
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
    float3 v_WorldNormal : TEXCOORD0;
};

struct DepthNormalPushConstants
{
    float4x4 ViewProj;  // camera world -> clip
};

[[vk::push_constant]] ConstantBuffer<DepthNormalPushConstants> g_PC;

[shader("vertex")]
VSOutput vsMain(VSInput input)
{
    CasterRecord caster = g_Casters[input.a_CandidateId];

    VSOutput output;
    output.sv_position = mul(g_PC.ViewProj, mul(caster.World, float4(input.a_Position, 1.0)));
    output.v_WorldNormal = caster.NormalColumn0.xyz * input.a_Normal.x +
                           caster.NormalColumn1.xyz * input.a_Normal.y +
                           caster.NormalColumn2.xyz * input.a_Normal.z;
    return output;
}
