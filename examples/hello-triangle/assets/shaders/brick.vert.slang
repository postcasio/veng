#include "material_data.slang"

// Forward material vertex stage. VSInput matches veng's
// canonical vertex layout (Mesh::CanonicalLayout / the core pack's canonical
// layout): position/normal/tangent[float4]/uv — the layout the cooked cube mesh
// is uploaded in.
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
    float2 v_UV : TEXCOORD0;
};

[shader("vertex")]
VSOutput vsMain(VSInput input)
{
    VSOutput output;
    output.sv_position = mul(g_PC.MVP, float4(input.a_Position, 1.0));
    output.v_UV = input.a_UV;
    return output;
}
