// Debug-draw line fragment stage. Samples the g-buffer depth at the dynamic-resolution
// sub-rect (remapping the logical UV by RenderScaleUV/MaxValidUV, identity at full res)
// and attenuates the line color when this fragment lies behind the scene geometry — the
// single-pass depth-aware occluded fade. The pass writes the LDR scene color after
// tonemap and is not hardware depth-tested; the depth sample is the only occlusion test.
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
    float4 v_Color : COLOR0;
    float2 v_ScreenUV : TEXCOORD0;
    float v_NdcDepth : TEXCOORD1;
};

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
    SamplerState samp = g_Samplers[NonUniformResourceIndex(g_PC.Sampler)];
    ViewConstants vc = LoadViewConstants(g_PC.ViewConstantsIndex);

    // The g-buffer depth lives in the dynamic-resolution sub-rect of its allocation, so the
    // logical screen UV maps through RenderScaleUV/MaxValidUV before the sample.
    float2 sampleUV = ScaledSampleUV(input.v_ScreenUV, vc.RenderScaleUV.xy, vc.MaxValidUV.xy);
    float sceneDepth =
        g_Textures[NonUniformResourceIndex(g_PC.DepthTexture)].Sample(samp, sampleUV).r;

    float4 color = input.v_Color;

    // A fragment whose depth is greater than the scene depth is behind geometry — occluded.
    // A small bias absorbs depth-buffer slop so a gizmo coplanar with a surface is not flagged.
    const float bias = 0.00005;
    if (input.v_NdcDepth > sceneDepth + bias)
    {
        color.rgb *= g_PC.OccludedFade;
        color.a *= g_PC.OccludedFade;
    }

    return color;
}
