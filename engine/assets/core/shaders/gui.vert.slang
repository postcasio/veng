// GUI vertex stage shared by both fragment paths. Transforms a framebuffer-pixel
// position into clip space through the push-constant inverse screen size (the UI
// image extent), and forwards the interpolants both fragment paths read: the UV,
// the linear straight-alpha color, the rounded-rect half-extent and the per-vertex
// signed local coordinate (both in pixels, the fragment reconstructs the SDF from
// their interpolation), and the packed params (corner radius, border width, and the
// bindless texture/sampler indices), and the shadow parameters (signed blur, spread,
// and offset) a shadow quad carries. Vulkan clip space has Y pointing down, so a
// top-left pixel (y=0) maps to clip y=-1 with no extra flip.

struct PushConstants
{
    float2 InvScreenSize; // 1 / UI image extent in pixels
    uint GradientBuffer;  // bindless g_Buffers slot of the gradient-record buffer (fragment reads it)
    uint GradientBase;    // record-index base for the current frame-in-flight (fragment reads it)
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

struct VSInput
{
    float2 a_Position : POSITION;
    float2 a_Uv : TEXCOORD0;
    float4 a_Color : COLOR0;
    float2 a_RectHalf : TEXCOORD1;
    float2 a_RectCoord : TEXCOORD2;
    float4 a_Params : TEXCOORD3;
    uint a_GradientSelector : TEXCOORD4;
    float4 a_Shadow : TEXCOORD5;
};

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_Uv : TEXCOORD0;
    float4 v_Color : COLOR0;
    float2 v_RectHalf : TEXCOORD1;
    float2 v_RectCoord : TEXCOORD2;
    nointerpolation float4 v_Params : TEXCOORD3;
    nointerpolation uint v_GradientSelector : TEXCOORD4;
    nointerpolation float4 v_Shadow : TEXCOORD5;
};

[shader("vertex")]
VSOutput vsMain(VSInput input)
{
    VSOutput output;
    float2 ndc = input.a_Position * g_PC.InvScreenSize * 2.0 - 1.0;
    output.sv_position = float4(ndc, 0.0, 1.0);
    output.v_Uv = input.a_Uv;
    output.v_Color = input.a_Color;
    output.v_RectHalf = input.a_RectHalf;
    output.v_RectCoord = input.a_RectCoord;
    output.v_Params = input.a_Params;
    output.v_GradientSelector = input.a_GradientSelector;
    output.v_Shadow = input.a_Shadow;
    return output;
}
