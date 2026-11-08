// A vertex stage reading vertex inputs and the instance index, which reaches Metal as the instance
// id with the base instance already folded in.
#include "Veng/binding.slang"

struct Push
{
    float4x4 ViewProj;
};
VE_PUSH_CONSTANTS(Push, g_PC);

struct VertexInput
{
    [[vk::location(0)]] float3 Position : POSITION;
    [[vk::location(1)]] float2 Uv : TEXCOORD0;
};

struct VertexOutput
{
    float4 Position : SV_Position;
    float2 Uv : TEXCOORD0;
    nointerpolation uint Instance : TEXCOORD1;
};

[shader("vertex")]
VertexOutput vsMain(VertexInput input, uint instance : SV_VulkanInstanceID)
{
    VertexOutput output;
    output.Position = mul(g_PC.ViewProj, float4(input.Position, 1.0));
    output.Uv = input.Uv;
    output.Instance = instance;
    return output;
}
