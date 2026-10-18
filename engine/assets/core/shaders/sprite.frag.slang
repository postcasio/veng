// Flipbook sprite fragment stage. A stepped unlit sprite samples its frame and resolves the sheet's
// alpha convention into premultiplied colour and coverage, then applies the tint and opacity. A
// blended sprite reads each texture at frame N and frame N+1, warped along the motion vectors when
// it blends by motion, and shades the blend once. A six-way sprite weights its two lightmaps by the
// light its vertex stage gathered, adds the ambient, and adds the emission its temperature reads
// through the ramp. A sprite with a soft distance then fades where it meets the opaque scene.
//
// fsMain returns the colour alone; fsMainMasked also returns the bloom-mask amplitude: the
// colour's luminance for an unlit sprite, so a bright sprite glows regardless of the bloom
// threshold, and the emission's for a six-way one, so a hot core glows and sunlit smoke does not.
// The alpha pipeline composites the result premultiplied-over and the additive pipeline adds it, so
// one stage serves both.
#include "view_constants.slang"

[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];

#include "sprite_record.slang"
#include "Veng/scene_depth.slang"

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
    float4 v_Color : COLOR0;
    nointerpolation uint v_Texture : TEXCOORD1;
    nointerpolation uint v_Sampler : TEXCOORD2;
    nointerpolation uint v_AlphaMode : TEXCOORD3;
    nointerpolation uint v_Sprite : TEXCOORD4;
    float2 v_Quad : TEXCOORD5;
    float v_ViewDepth : TEXCOORD6;
    nointerpolation float3 v_In0 : TEXCOORD7;
    nointerpolation float3 v_In1 : TEXCOORD8;
    nointerpolation float3 v_In2 : TEXCOORD9;
    nointerpolation float3 v_In3 : TEXCOORD10;
    nointerpolation float3 v_In4 : TEXCOORD11;
    nointerpolation float3 v_In5 : TEXCOORD12;
    nointerpolation float3 v_Ambient : TEXCOORD13;
};

struct MaskedOutput
{
    float4 Color : SV_Target0;
    float Mask : SV_Target1;
};

// The reactive variants add the temporal resolve's reactive mask after any bloom mask: the pixel's
// coverage (SpriteReactiveCoverage), so a resolve takes more of this frame where the draw stands.
struct ReactiveOutput
{
    float4 Color : SV_Target0;
    float Reactive : SV_Target1;
};

struct MaskedReactiveOutput
{
    float4 Color : SV_Target0;
    float Mask : SV_Target1;
    float Reactive : SV_Target2;
};

struct SpriteShade
{
    float4 Color;
    float Mask;
};

SpriteShade ShadeSprite(VSOutput input)
{
    GpuSprite sprite = g_Sprites[input.v_Sprite];
    uint blend = SpriteFrameBlendOf(sprite);
    bool sixWay = (sprite.Flags & SpriteFlagSixWay) != 0u;
    // Taken before any branch: the shading mode is flat per primitive, but derivatives are only
    // defined in uniform control flow across the quad.
    float2 dqdx = ddx(input.v_Quad);
    float2 dqdy = ddy(input.v_Quad);

    SpriteShade shade;
    if (!sixWay && blend == SpriteBlendStep)
    {
        float4 texel = g_Textures[NonUniformResourceIndex(input.v_Texture)].Sample(
            g_Samplers[NonUniformResourceIndex(input.v_Sampler)], input.v_UV);
        shade.Color = ShadeUnlitSprite(texel, input.v_AlphaMode, input.v_Color.rgb,
                                       input.v_Color.a);
        shade.Mask = saturate(dot(shade.Color.rgb, SpriteLumaWeights));
    }
    else
    {
        SamplerState atlasSampler = g_Samplers[NonUniformResourceIndex(sprite.Sampler)];
        SpriteFramePair pair = SpriteFramePairOf(sprite);
        SpriteFrameTaps taps =
            SpriteFrameTapsAt(pair, input.v_Quad, dqdx, dqdy, float2(0.0), float2(0.0));
        if (blend == SpriteBlendMotion)
        {
            Texture2D motion = g_Textures[NonUniformResourceIndex(sprite.Motion)];
            float2 storedN = motion.SampleGrad(atlasSampler, taps.UvN, taps.DxN, taps.DyN).rg;
            float2 storedN1 =
                motion.SampleGrad(atlasSampler, taps.UvN1, taps.DxN1, taps.DyN1).rg;
            taps = SpriteFrameTapsAt(pair, input.v_Quad, dqdx, dqdy,
                                     DecodeMotion(storedN, sprite.MotionScale),
                                     DecodeMotion(storedN1, sprite.MotionScale));
        }

        float4 primary = SampleFrameBlended(g_Textures[NonUniformResourceIndex(sprite.Texture)],
                                            atlasSampler, taps);
        if (!sixWay)
        {
            shade.Color =
                ShadeUnlitSprite(primary, sprite.AlphaMode, sprite.Color.rgb, sprite.Color.a);
            shade.Mask = saturate(dot(shade.Color.rgb, SpriteLumaWeights));
        }
        else
        {
            float4 lightB = SampleFrameBlended(g_Textures[NonUniformResourceIndex(sprite.LightB)],
                                               atlasSampler, taps);
            float4 temperature = SampleFrameBlended(
                g_Textures[NonUniformResourceIndex(sprite.Temperature)], atlasSampler, taps);
            float stored =
                (sprite.Flags & SpriteFlagTemperatureRed) != 0u ? temperature.r : temperature.a;
            float t = SpriteTemperature(stored, (sprite.Flags & SpriteFlagSqrtTemperature) != 0u,
                                        sprite.TemperatureOffset);
            // The six-way branch is uniform across the quad (flat per primitive), so fwidth is
            // defined here.
            float gateWidth = fwidth(t);

            SpriteLightGather gather;
            gather.In[0] = input.v_In0;
            gather.In[1] = input.v_In1;
            gather.In[2] = input.v_In2;
            gather.In[3] = input.v_In3;
            gather.In[4] = input.v_In4;
            gather.In[5] = input.v_In5;
            gather.Ambient = input.v_Ambient;
            SpriteEmissionRamp ramp = (sprite.Flags & SpriteFlagEmits) != 0u
                                          ? g_SpriteRamps[sprite.Ramp]
                                          : SpriteEmissionRampNone();

            SixWayShade sixWayShade =
                ShadeSixWay(primary, lightB.rgb, t, gateWidth, gather, ramp, sprite.Color.rgb,
                            sprite.Color.a, sprite.EmissionIntensity);
            shade.Color = sixWayShade.Color;
            shade.Mask =
                saturate(dot(sixWayShade.Emission, SpriteLumaWeights)) * sprite.Color.a;
        }
    }

    if (sprite.SoftDistance > 0.0)
    {
        ViewConstants vc = LoadViewConstants(g_PC.ViewConstantsIndex);
        if (SceneDepthAvailable(vc))
        {
            float2 screenUV = SceneScreenUV(vc, input.sv_position);
            float sceneDepth = ViewDepthFromDeviceDepth(vc, SampleSceneDepth(vc, screenUV));
            float fade = SoftEdgeFade(sceneDepth - input.v_ViewDepth, sprite.SoftDistance);
            shade.Color *= fade;
            shade.Mask *= fade;
        }
    }
    return shade;
}

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
    return ShadeSprite(input).Color;
}

[shader("fragment")]
MaskedOutput fsMainMasked(VSOutput input)
{
    SpriteShade shade = ShadeSprite(input);
    MaskedOutput output;
    output.Color = shade.Color;
    output.Mask = shade.Mask;
    return output;
}

[shader("fragment")]
ReactiveOutput fsMainReactive(VSOutput input)
{
    SpriteShade shade = ShadeSprite(input);
    ReactiveOutput output;
    output.Color = shade.Color;
    output.Reactive = SpriteReactiveCoverage(shade.Color);
    return output;
}

[shader("fragment")]
MaskedReactiveOutput fsMainMaskedReactive(VSOutput input)
{
    SpriteShade shade = ShadeSprite(input);
    MaskedReactiveOutput output;
    output.Color = shade.Color;
    output.Mask = shade.Mask;
    output.Reactive = SpriteReactiveCoverage(shade.Color);
    return output;
}
