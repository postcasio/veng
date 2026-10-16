// The motion MetalFX's temporal scaler reconstructs from: the g-buffer velocity wherever geometry
// stands, and the camera's turn wherever the surface pass left the cleared depth. The engine's own
// resolve reprojects the background the same way (taa_resolve's direction branch); without it the
// cleared velocity would tell the scaler the sky and everything drawn over it without writing
// motion — translucents, point fields — never moved, and a turning view would smear them.
//
// Written at the rendered sub-rect in the velocity channel's convention, current minus previous
// position in viewport UV, so the scaler's motion scale is the one the velocity channel takes.
#include "view_constants.slang"

[[vk::binding(0, 0)]] Texture2D g_Textures[];

struct PushConstants
{
    uint DepthTexture;       // the g-buffer depth (bindless index)
    uint VelocityTexture;    // the g-buffer velocity channel (bindless index)
    uint ViewConstantsIndex; // current frame-in-flight's view-constants region
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target0
{
    const int2 texel = int2(input.sv_position.xy);
    // Reverse-Z: the cleared depth is zero.
    if (g_Textures[NonUniformResourceIndex(g_PC.DepthTexture)].Load(int3(texel, 0)).r > 0.0)
    {
        return float4(
            g_Textures[NonUniformResourceIndex(g_PC.VelocityTexture)].Load(int3(texel, 0)).rg, 0.0,
            0.0);
    }

    ViewConstants vc = LoadViewConstants(g_PC.ViewConstantsIndex);
    const float2 uv = input.sv_position.xy / vc.ExtentParams.xy;
    const float4 prevClip = mul(vc.PrevFromCurDirection, float4(uv * 2.0 - 1.0, 0.0, 1.0));
    // A direction that was behind the previous view has no previous position to point at.
    if (prevClip.w <= 0.0)
    {
        return float4(0.0);
    }
    const float2 prevUv = (prevClip.xy / prevClip.w) * 0.5 + 0.5;
    return float4(uv - prevUv, 0.0, 0.0);
}
