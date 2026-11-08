// ImGui vertex stage. Maps a draw list's display-space position into clip space through the
// push-constant scale and translate (2 / display size, and the offset that puts the display's
// top-left corner at clip -1), and forwards the vertex colour and texture coordinate. Vulkan clip
// space has Y pointing down, so the display's top row maps to clip y = -1 with no flip.
//
// The vertex stream is ImGui's own ImDrawVert: a float2 position, a float2 UV, and an RGBA8 colour
// the input assembler normalizes to float4.

#include "Veng/binding.slang"

struct PushConstants
{
    float2 Scale;
    float2 Translate;
    uint Texture; // bindless g_Textures slot (fragment reads it)
    uint Sampler; // bindless g_Samplers slot (fragment reads it)
};

VE_PUSH_CONSTANTS(PushConstants, g_PC);

struct VSInput
{
    float2 a_Position : POSITION;
    float2 a_UV : TEXCOORD0;
    float4 a_Color : COLOR0;
};

struct VSOutput
{
    float4 sv_position : SV_Position;
    float4 v_Color : COLOR0;
    float2 v_UV : TEXCOORD0;
};

[shader("vertex")]
VSOutput vsMain(VSInput input)
{
    VSOutput output;
    output.sv_position = float4(input.a_Position * g_PC.Scale + g_PC.Translate, 0.0, 1.0);
    output.v_Color = input.a_Color;
    output.v_UV = input.a_UV;
    return output;
}
