// Point-field resolved-sprite fragment stage. Draws a soft round emissive point (a radial
// falloff over the quad), attenuated when the point lies behind the scene geometry — the same
// single-pass depth-aware occluded fade the debug billboards use, sampling the g-buffer depth
// through the dynamic-resolution sub-rect remap. Additive output, so overlapping points sum.
#include "../view_constants.slang"

[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];

struct PushConstants
{
    uint DepthTexture;
    uint Sampler;
    uint ViewConstantsIndex;
    uint FirstPoint;
    uint PointCount;
    float MinPixels;
    float MaxPixels;
    float MaxIntensity;
    float ViewportWidth;
    float ViewportHeight;
    float Opacity;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
    float4 v_Color : COLOR0;
    float2 v_ScreenUV : TEXCOORD1;
    float v_NdcDepth : TEXCOORD2;
};

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
    SamplerState samp = g_Samplers[NonUniformResourceIndex(g_PC.Sampler)];
    ViewConstants vc = LoadViewConstants(g_PC.ViewConstantsIndex);

    // Radial falloff over the [-0.5,0.5] quad: bright center, zero at the disc edge.
    float r = length(input.v_UV) * 2.0;
    float falloff = saturate(1.0 - r);
    falloff *= falloff;
    if (falloff <= 0.0)
    {
        discard;
    }

    float3 color = input.v_Color.rgb * input.v_Color.a * falloff * g_PC.Opacity;

    // Depth-aware occluded fade: sample the g-buffer depth at the dynamic-resolution sub-rect.
    float2 sampleUV = ScaledSampleUV(input.v_ScreenUV, vc.RenderScaleUV.xy, vc.MaxValidUV.xy);
    float sceneDepth =
        g_Textures[NonUniformResourceIndex(g_PC.DepthTexture)].Sample(samp, sampleUV).r;
    const float bias = 0.00005;
    if (input.v_NdcDepth > sceneDepth + bias)
    {
        color *= 0.35;
    }

    return float4(color, 1.0);
}
