// Particle fragment stage. A billboard shades exactly as a sprite does, through the shared sprite
// shading core: an unlit flipbook samples its frame (or blends frames N and N+1, warped along the
// motion vectors when it blends by motion) and resolves its alpha convention; a six-way flipbook
// weights its two lightmaps by the light the update gathered for the particle, adds the ambient,
// and adds the emission its temperature reads through the ramp. A streak is its colour times its
// tint, falling off as (1 - v^2)^2 across its width and softly toward its ends. A particle with a
// soft distance then fades where it meets the opaque scene.
//
// fsMain returns the colour alone; fsMainMasked also returns the bloom-mask amplitude: the
// colour's luminance for an unlit billboard or a streak, the emission's for a six-way billboard.
// The result is premultiplied, so the alpha pipeline composites it premultiplied-over and the
// additive pipeline adds it.
#include "view_constants.slang"

#include "Veng/bindless.slang"

#include "particle_draw.slang"
#include "Veng/scene_depth.slang"

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

struct ParticleShade
{
    float4 Color;
    float Mask;
};

ParticleShade ShadeParticle(VSOutput input)
{
    GpuParticleDraw draw = g_Set3.Draws[input.v_Draw];
    GpuParticleEmitter emitter = g_Set3.Emitters[draw.Emitter];
    float2 tintRg = UnpackHalf2(draw.TintOpacity.x);
    float2 tintBa = UnpackHalf2(draw.TintOpacity.y);
    float3 tint = float3(tintRg, tintBa.x);
    float opacity = tintBa.y;
    // Taken before any branch: the render mode is flat per primitive, but derivatives are only
    // defined in uniform control flow across the quad.
    float2 dqdx = ddx(input.v_Quad);
    float2 dqdy = ddy(input.v_Quad);

    ParticleShade shade;
    if (emitter.Mode.x == ParticleRenderStreak)
    {
        float falloff;
        if (input.v_Round != 0u)
        {
            float r2 = saturate(dot(input.v_Quad, input.v_Quad));
            falloff = (1.0 - r2) * (1.0 - r2);
        }
        else
        {
            float v2 = saturate(input.v_Quad.y * input.v_Quad.y);
            float across = (1.0 - v2) * (1.0 - v2);
            float along = saturate(min(input.v_Quad.x, 1.0 - input.v_Quad.x) * 8.0);
            falloff = across * along;
        }
        float coverage = opacity * falloff * input.v_Coverage;
        shade.Color = float4(tint * coverage, coverage);
        shade.Mask = saturate(dot(shade.Color.rgb, SpriteLumaWeights));
    }
    else
    {
        bool sixWay = (emitter.Mode.w & ParticleFlagSixWay) != 0u;
        uint blend = ParticleFrameBlendOf(emitter);
        SamplerState atlasSampler = g_Samplers[NonUniformIndex(emitter.Grid.w)];
        SpriteFramePair pair = ParticleFramePair(emitter, draw);
        SpriteFrameTaps taps =
            SpriteFrameTapsAt(pair, input.v_Quad, dqdx, dqdy, float2(0.0), float2(0.0));
        if (blend == ParticleBlendMotion)
        {
            Texture2D motion = g_Textures[NonUniformIndex(emitter.Textures.z)];
            float2 storedN = motion.SampleGrad(atlasSampler, taps.UvN, taps.DxN, taps.DyN).rg;
            float2 storedN1 =
                motion.SampleGrad(atlasSampler, taps.UvN1, taps.DxN1, taps.DyN1).rg;
            taps = SpriteFrameTapsAt(pair, input.v_Quad, dqdx, dqdy,
                                     DecodeMotion(storedN, emitter.Playback.y),
                                     DecodeMotion(storedN1, emitter.Playback.y));
        }

        float4 primary = SampleFrameBlended(
            g_Textures[NonUniformIndex(emitter.Textures.x)], atlasSampler, taps);
        if (!sixWay)
        {
            shade.Color = ShadeUnlitSprite(primary, emitter.Counts.z, tint, opacity);
            shade.Mask = saturate(dot(shade.Color.rgb, SpriteLumaWeights));
        }
        else
        {
            float4 lightB = SampleFrameBlended(
                g_Textures[NonUniformIndex(emitter.Textures.y)], atlasSampler, taps);
            float4 temperature = SampleFrameBlended(
                g_Textures[NonUniformIndex(emitter.Textures.w)], atlasSampler, taps);
            float stored =
                (emitter.Mode.w & ParticleFlagTemperatureRed) != 0u ? temperature.r : temperature.a;
            float t = SpriteTemperature(stored, (emitter.Mode.w & ParticleFlagSqrtTemperature) != 0u,
                                        draw.TemperatureOffset);
            // The six-way branch is uniform across the quad (flat per primitive).
            float gateWidth = fwidth(t);
            SpriteLightGather gather = UnpackParticleLight(draw.Light0, draw.Light1, draw.Light2);
            SpriteEmissionRamp ramp = (emitter.Mode.w & ParticleFlagEmits) != 0u
                                          ? emitter.Ramp
                                          : SpriteEmissionRampNone();
            SixWayShade sixWayShade = ShadeSixWay(primary, lightB.rgb, t, gateWidth, gather, ramp,
                                                  tint, opacity, emitter.Noise.w);
            shade.Color = sixWayShade.Color;
            shade.Mask = saturate(dot(sixWayShade.Emission, SpriteLumaWeights)) * opacity;
        }
    }

    if (draw.SoftDistance > 0.0)
    {
        ViewConstants vc = LoadViewConstants(g_PC.ViewConstantsIndex);
        if (SceneDepthAvailable(vc))
        {
            float2 screenUV = SceneScreenUV(vc, input.sv_position);
            float sceneDepth = ViewDepthFromDeviceDepth(vc, SampleSceneDepth(vc, screenUV));
            float fade = SoftEdgeFade(sceneDepth - input.v_ViewDepth, draw.SoftDistance);
            shade.Color *= fade;
            shade.Mask *= fade;
        }
    }
    return shade;
}

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
    return ShadeParticle(input).Color;
}

[shader("fragment")]
MaskedOutput fsMainMasked(VSOutput input)
{
    ParticleShade shade = ShadeParticle(input);
    MaskedOutput output;
    output.Color = shade.Color;
    output.Mask = shade.Mask;
    return output;
}

[shader("fragment")]
ReactiveOutput fsMainReactive(VSOutput input)
{
    ParticleShade shade = ShadeParticle(input);
    ReactiveOutput output;
    output.Color = shade.Color;
    output.Reactive = SpriteReactiveCoverage(shade.Color);
    return output;
}

[shader("fragment")]
MaskedReactiveOutput fsMainMaskedReactive(VSOutput input)
{
    ParticleShade shade = ShadeParticle(input);
    MaskedReactiveOutput output;
    output.Color = shade.Color;
    output.Mask = shade.Mask;
    output.Reactive = SpriteReactiveCoverage(shade.Color);
    return output;
}
