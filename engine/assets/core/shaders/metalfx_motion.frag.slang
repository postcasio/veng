// The motion and the colour MetalFX's temporal scaler reconstructs from.
//
// The motion is the g-buffer velocity wherever geometry stands, and the camera's turn wherever the
// surface pass left the cleared depth. The engine's own resolve reprojects the background the same
// way (taa_resolve's direction branch); without it the cleared velocity would tell the scaler the
// sky and everything drawn over it without writing motion — translucents, point fields — never
// moved, and a turning view would smear them.
//
// Written at the rendered sub-rect in the velocity channel's convention, current minus previous
// position in viewport UV, so the scaler's motion scale is the one the velocity channel takes.
//
// The colour is the lit target with every texel the tone curve cannot show made safe: a non-finite
// channel zeroed, a negative one clamped to zero, and a luminance past half the firefly ceiling
// rolled off along the bloom bright pass's shoulder toward the ceiling, scaled as a whole so its
// hue is kept. The engine's own resolve damps such texels by weighting each sample by its exposed
// luminance and clipping history to the neighbourhood; the scaler does neither, and reconstructs a
// lone spike or a non-finite channel into a saturated pixel the tonemap shows in one hue.
#include "view_constants.slang"

[[vk::binding(0, 0)]] Texture2D g_Textures[];

struct PushConstants
{
    uint DepthTexture;       // the g-buffer depth (bindless index)
    uint VelocityTexture;    // the g-buffer velocity channel (bindless index)
    uint ViewConstantsIndex; // current frame-in-flight's view-constants region
    uint LitTexture;         // the jittered lit target (bindless index)
    float FireflyCeiling;    // the luminance the colour is rolled off toward, HDR units
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

struct FSOutput
{
    float4 Motion : SV_Target0;
    float4 Color : SV_Target1;
};

float2 Motion(int2 texel, float2 position)
{
    // Reverse-Z: the cleared depth is zero.
    if (g_Textures[NonUniformResourceIndex(g_PC.DepthTexture)].Load(int3(texel, 0)).r > 0.0)
    {
        return g_Textures[NonUniformResourceIndex(g_PC.VelocityTexture)].Load(int3(texel, 0)).rg;
    }

    ViewConstants vc = LoadViewConstants(g_PC.ViewConstantsIndex);
    const float2 uv = position / vc.ExtentParams.xy;
    const float4 prevClip = mul(vc.PrevFromCurDirection, float4(uv * 2.0 - 1.0, 0.0, 1.0));
    // A direction that was behind the previous view has no previous position to point at.
    if (prevClip.w <= 0.0)
    {
        return float2(0.0);
    }
    const float2 prevUv = (prevClip.xy / prevClip.w) * 0.5 + 0.5;
    return uv - prevUv;
}

float3 Condition(float3 c)
{
    // Tested on the exponent bits rather than through isnan/isinf, which a fast-math compile is
    // free to fold away.
    const uint3 exponent = asuint(c) & 0x7F800000u;
    c = select(exponent == 0x7F800000u, float3(0.0), c);
    c = max(c, float3(0.0));
    const float knee = 0.5 * max(g_PC.FireflyCeiling, 1e-4);
    const float luma = dot(c, float3(0.2126, 0.7152, 0.0722));
    if (luma <= knee)
    {
        return c;
    }
    // The shoulder has unit slope at the knee and approaches knee + knee, the ceiling.
    const float over = luma - knee;
    const float limited = knee + over / (1.0 + over / knee);
    return c * (limited / luma);
}

[shader("fragment")]
FSOutput fsMain(VSOutput input)
{
    const int2 texel = int2(input.sv_position.xy);
    FSOutput output;
    output.Motion = float4(Motion(texel, input.sv_position.xy), 0.0, 0.0);
    const float4 lit = g_Textures[NonUniformResourceIndex(g_PC.LitTexture)].Load(int3(texel, 0));
    output.Color = float4(Condition(lit.rgb), lit.a);
    return output;
}
