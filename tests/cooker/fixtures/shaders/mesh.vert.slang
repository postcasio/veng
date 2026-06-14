struct VSInput
{
    float3 a_Position : POSITION;
    float3 a_Normal : NORMAL;
    float3 a_Tangent : TANGENT;
    float2 a_UV : TEXCOORD0;
};

struct VSOutput
{
    float4 sv_position : SV_Position;
    float3 v_Normal : NORMAL;
    float2 v_UV : TEXCOORD0;
};

struct PushConstants
{
    float4x4 MVP;
    float4x4 Model;
};

[[vk::push_constant]]
ConstantBuffer<PushConstants> g_PushConstants;

[shader("vertex")]
VSOutput vsMain(VSInput input)
{
    VSOutput output;
    output.sv_position = mul(g_PushConstants.MVP, float4(input.a_Position, 1.0));
    output.v_Normal = input.a_Normal;
    output.v_UV = input.a_UV;
    return output;
}
