// Depth-only shadow vertex stage. Renders the scene's opaque meshes from the
// light-space view-projection into the shadow map; no fragment stage and no
// color output, only depth. VSInput matches veng's canonical vertex layout
// (Mesh::CanonicalLayout) so it draws the same vertex buffers as the geometry
// pass. The push block carries only the light-space MVP (offset 0) — a 64-byte
// vertex push range, no material/normal-matrix block, so the depth-only pipeline
// layout is minimal.
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
};

struct ShadowPushConstants
{
    float4x4 MVP;  // light-space world → clip
};

[[vk::push_constant]] ConstantBuffer<ShadowPushConstants> g_PC;

[shader("vertex")]
VSOutput vsMain(VSInput input)
{
    VSOutput output;
    output.sv_position = mul(g_PC.MVP, float4(input.a_Position, 1.0));
    return output;
}
