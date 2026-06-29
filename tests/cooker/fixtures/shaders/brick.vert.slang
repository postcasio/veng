// Deferred material vertex stage (cooker fixture). VSInput matches the fixture
// canonical layout 7001 (position/normal/tangent[float3]/uv). The normal is carried to
// world space by the push-constant NormalMatrix for the g-buffer write.
//
// Self-contained like its brick.frag sibling: it declares its own MVP/NormalMatrix push
// block, not the engine's DrawData model — it exercises the cook machinery generically.

// One shared push-constant block, used by both stages (see brick.frag). The vertex stage
// reads MVP (offset 0) and NormalMatrix (offset 80); the block is exactly 128 bytes.
struct PushConstants
{
    float4x4 MVP;          // offset 0
    uint MaterialIndex;    // offset 64
    uint Pad0;             // offset 68
    uint Pad1;             // offset 72
    uint Pad2;             // offset 76
    float3x3 NormalMatrix; // offset 80 (48 bytes) — ends at 128
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

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
    float2 v_UV : TEXCOORD0;
    float3 v_WorldNormal : NORMAL;
};

[shader("vertex")]
VSOutput vsMain(VSInput input)
{
    VSOutput output;
    output.sv_position = mul(g_PC.MVP, float4(input.a_Position, 1.0));
    output.v_UV = input.a_UV;
    output.v_WorldNormal = mul(g_PC.NormalMatrix, input.a_Normal);
    return output;
}
