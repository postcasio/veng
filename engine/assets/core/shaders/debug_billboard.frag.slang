// Debug-draw billboard fragment stage. Samples the per-instance icon texture (bindless),
// tints it by the billboard color, and attenuates the result when the quad lies behind the
// scene geometry — the same single-pass depth-aware occluded fade as the debug lines.
#include "view_constants.slang"

[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];

struct PushConstants
{
    uint DepthTexture;
    uint Sampler;
    uint ViewConstantsIndex;
    uint Pad0;
    float OccludedFade;
    uint Pad1;
    uint Pad2;
    uint Pad3;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
    float4 v_Color : COLOR0;
    float2 v_ScreenUV : TEXCOORD1;
    float v_NdcDepth : TEXCOORD2;
    nointerpolation uint v_Texture : TEXCOORD3;
};

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
    SamplerState samp = g_Samplers[NonUniformResourceIndex(g_PC.Sampler)];
    ViewConstants vc = LoadViewConstants(g_PC.ViewConstantsIndex);

    float4 texel = g_Textures[NonUniformResourceIndex(input.v_Texture)].Sample(samp, input.v_UV);
    float4 color = texel * input.v_Color;

    // Sample the g-buffer depth at the dynamic-resolution sub-rect and fade behind geometry.
    float2 sampleUV = ScaledSampleUV(input.v_ScreenUV, vc.RenderScaleUV.xy, vc.MaxValidUV.xy);
    float sceneDepth =
        g_Textures[NonUniformResourceIndex(g_PC.DepthTexture)].Sample(samp, sampleUV).r;

    const float bias = 0.00005;
    if (input.v_NdcDepth > sceneDepth + bias)
    {
        color.rgb *= g_PC.OccludedFade;
        color.a *= g_PC.OccludedFade;
    }

    return color;
}
