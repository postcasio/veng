// Debug-draw line vertex stage. Transforms a world-space line endpoint through the
// per-frame view-projection (the unjittered CurViewProj from the set-0 view constants),
// passing the linear-RGBA color, the logical screen UV, and the fragment's NDC depth to
// the fragment for the depth-aware occlusion fade.
#include "view_constants.slang"

struct VSInput
{
    float3 a_Position : POSITION;
    float4 a_Color : COLOR0;
};

struct PushConstants
{
    uint DepthTexture;        // g-buffer depth bindless index
    uint Sampler;             // shared sampler bindless index
    uint ViewConstantsIndex;  // current frame-in-flight's view-constants region
    uint Pad0;
    float OccludedFade;       // color scale applied to an occluded fragment (0..1)
    uint Pad1;
    uint Pad2;
    uint Pad3;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float4 v_Color : COLOR0;
    float2 v_ScreenUV : TEXCOORD0;  // logical [0,1] screen UV for the depth sample
    float v_NdcDepth : TEXCOORD1;   // this fragment's NDC depth, for the occlusion compare
};

[shader("vertex")]
VSOutput vsMain(VSInput input)
{
    ViewConstants view = LoadViewConstants(g_PC.ViewConstantsIndex);

    float4 clip = mul(view.CurViewProj, float4(input.a_Position, 1.0));

    VSOutput output;
    output.sv_position = clip;
    output.v_Color = input.a_Color;
    // Logical screen UV from NDC (clip / w → [-1,1] → [0,1]). Vulkan clip-space Y points
    // down, so UV.y maps directly without a flip (matching the g-buffer sample sites).
    output.v_ScreenUV = (clip.xy / clip.w) * 0.5 + 0.5;
    output.v_NdcDepth = clip.z / clip.w;
    return output;
}
