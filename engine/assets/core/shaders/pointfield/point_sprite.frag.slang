// Point-field resolved-sprite fragment stage. Draws a soft round emissive point (a radial
// falloff over the quad). Additive output, so overlapping points sum.
//
// VE_POINT_DEPTH_FADE selects a compile-time variant. When defined (the default, the
// point_sprite.frag wrapper's shader), the point is attenuated where it lies behind the scene
// geometry — the depth-aware occluded fade the debug billboards use, sampling the g-buffer depth
// at the dynamic-resolution sub-rect UV the vertex stage hoisted (v_SampleUV). When zero (the
// point_sprite_nofade.frag wrapper), the depth sample, its sub-rect remap, and the occluded-fade
// compare are dropped entirely — right for a field composited over background with no occluding
// geometry, where the fade can never trigger, so it pays no bindless depth sample per fragment.
#ifndef VE_POINT_DEPTH_FADE
#define VE_POINT_DEPTH_FADE 1
#endif

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
    float2 v_SampleUV : TEXCOORD1;
    float v_NdcDepth : TEXCOORD2;
};

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
    // Radial falloff over the [-0.5,0.5] quad: bright center, zero at the disc edge.
    float r = length(input.v_UV) * 2.0;
    float falloff = saturate(1.0 - r);
    falloff *= falloff;
    if (falloff <= 0.0)
    {
        discard;
    }

    float3 color = input.v_Color.rgb * input.v_Color.a * falloff * g_PC.Opacity;

#if VE_POINT_DEPTH_FADE
    // Depth-aware occluded fade: sample the g-buffer depth at the sub-rect UV the vertex stage
    // remapped (v_SampleUV). The push-constant depth/sampler indices are per-draw uniform, so the
    // descriptor index cannot diverge — no NonUniformResourceIndex.
    SamplerState samp = g_Samplers[g_PC.Sampler];
    float sceneDepth = g_Textures[g_PC.DepthTexture].Sample(samp, input.v_SampleUV).r;
    const float bias = 0.00005;
    if (input.v_NdcDepth > sceneDepth + bias)
    {
        color *= 0.35;
    }
#endif

    return float4(color, 1.0);
}
