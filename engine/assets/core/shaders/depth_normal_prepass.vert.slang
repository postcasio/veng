// Camera depth + world-normal prepass vertex stage (static/canonical layout). Rasterizes
// opaque geometry from the camera view and passes only the clip position and the world-space
// normal — the lean geometry render path's static variant. Unlike the depth-only shadow caster
// (shadow_depth.vert), this passes a normal varying and widens the push block with a world normal
// matrix, so the one-MRT fragment can write a world normal the vertex stage never interpolated
// for a shadow. VSInput matches veng's canonical vertex layout (Mesh::CanonicalLayout) so it draws
// the same vertex buffers as the g-buffer pass; there is no material sample.
struct VSInput
{
    float3 a_Position : POSITION;
    float3 a_Normal : NORMAL;
    float4 a_Tangent : TANGENT;
    float2 a_UV : TEXCOORD0;
};

struct VSOutput
{
    float4 sv_position : SV_Position;
    float3 v_WorldNormal : TEXCOORD0;
};

// The world normal matrix (inverse-transpose of World's upper 3x3) is carried as three column
// vectors rather than a float3x3/float4x4, matching Veng/surface_vertex.slang and keeping the push
// block within the 128-byte range (a second float4x4 would overflow it on the skinned variant).
struct DepthNormalPushConstants
{
    float4x4 MVP;         // camera world -> clip
    float4 NormalColumn0; // world normal matrix, column 0 (xyz)
    float4 NormalColumn1; // column 1
    float4 NormalColumn2; // column 2
};

[[vk::push_constant]] ConstantBuffer<DepthNormalPushConstants> g_PC;

[shader("vertex")]
VSOutput vsMain(VSInput input)
{
    VSOutput output;
    output.sv_position = mul(g_PC.MVP, float4(input.a_Position, 1.0));
    output.v_WorldNormal = g_PC.NormalColumn0.xyz * input.a_Normal.x +
                           g_PC.NormalColumn1.xyz * input.a_Normal.y +
                           g_PC.NormalColumn2.xyz * input.a_Normal.z;
    return output;
}
