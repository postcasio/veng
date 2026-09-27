// GUI vertex stage shared by both fragment paths. Transforms a framebuffer-pixel
// position into clip space through the push-constant inverse screen size (the UI
// image extent), and forwards the interpolants both fragment paths read: the UV,
// the linear straight-alpha color, the rounded-rect half-extent and the per-vertex
// signed local coordinate (both in pixels, the fragment reconstructs the SDF from
// their interpolation), and the packed params (corner radius, border width, and the
// bindless texture/sampler indices), and the shadow parameters (signed blur, spread,
// and offset) a shadow quad carries, the UV sub-rect a nine-slice cell wraps its
// sampling within, and the arc silhouette a quad is masked to (its start, sweep,
// thickness, and signed radius, plus the vertex's coordinate from the arc's centre).
// Vulkan clip space has Y pointing down, so a top-left pixel (y=0)
// maps to clip y=-1 with no extra flip.

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
    float4 a_UvWrap : TEXCOORD6;
    float4 a_Arc : TEXCOORD7;
    float2 a_ArcCoord : TEXCOORD8;
};

// Varying locations follow declaration order, not the semantics, so a fragment may omit only a
// trailing run of these: the lanes the GuiFill domain and the MSDF path read come first, and the
// gradient selector, shadow, and UV wrap only the shape path reads come last.
struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_Uv : TEXCOORD0;
    float4 v_Color : COLOR0;
    float2 v_RectHalf : TEXCOORD1;
    float2 v_RectCoord : TEXCOORD2;
    nointerpolation float4 v_Params : TEXCOORD3;
    nointerpolation float4 v_Arc : TEXCOORD7;
    float2 v_ArcCoord : TEXCOORD8;
    nointerpolation uint v_GradientSelector : TEXCOORD4;
    nointerpolation float4 v_Shadow : TEXCOORD5;
    nointerpolation float4 v_UvWrap : TEXCOORD6;
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
    output.v_UvWrap = input.a_UvWrap;
    output.v_Arc = input.a_Arc;
    output.v_ArcCoord = input.a_ArcCoord;
    return output;
}
