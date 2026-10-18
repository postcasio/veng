// The binding-free half of the flipbook shading model: everything that turns a flipbook's stored
// texels into a premultiplied HDR colour once the light reaching it is known. Every function here
// is a function of its arguments alone — plain values, and for the frame blend a texture and
// sampler the caller hands in — and declares, reads and assumes no binding, no view block and no
// stage built-in. So a pass drawing sprites, a pass drawing particles, and a tool previewing a
// six-way bake under a light of its own all shade through the same arithmetic.
// Veng/sprite_shading.slang adds the half that reads the scene: the per-sprite light gather and
// the ambient term.
//
// The conventions:
// - A six-way flipbook holds two lightmaps. lightA's RGB is the light received from +X, +Y and -Z,
//   and its alpha the coverage; lightB's RGB is the light received from -X, -Y and +Z. The axes are
//   the sprite's own frame: X right, Y up, Z toward the viewer.
// - A lightmap texel is the fraction of a white light arriving along that axis that reaches the
//   eye, premultiplied by coverage. A response of 1 is a white diffuse card facing the light, whose
//   radiance under irradiance E is E/pi — which is why the gathered terms carry the 1/pi.
// - Temperature is normalised to [0, 1] and stored under a transfer (DecodeTemperature). Emission
//   is the ramp's colour at that temperature, gated off below an ignition threshold, times
//   coverage.
// - Motion vectors are offsets that carry a texel of frame N to its place in frame N+1, in units of
//   the flipbook's full frame extent, with v running down the frame.

#pragma once

static const float SpriteShadingPi = 3.14159265358979323846;

// The most keys an emission ramp holds, matching the renderer's MaxFlipbookEmissionKeys.
static const uint SpriteEmissionMaxKeys = 8;

// ---------------------------------------------------------------------------------------------
// Six-way lighting
// ---------------------------------------------------------------------------------------------

// The light a six-way flipbook receives along each of its six axes, and the ambient term.
struct SpriteLightGather
{
    // The radiance arriving along each axis, already divided by pi: In[0..2] weight lightA's
    // +X, +Y and -Z maps, In[3..5] lightB's -X, -Y and +Z maps.
    float3 In[6];
    // The ambient radiance a white diffuse surface receives, averaged over the six axes; it
    // multiplies the six maps' mean.
    float3 Ambient;
};

// A gather holding no light.
SpriteLightGather SpriteLightGatherNone()
{
    SpriteLightGather gather;
    for (uint i = 0; i < 6; ++i)
    {
        gather.In[i] = float3(0.0);
    }
    gather.Ambient = float3(0.0);
    return gather;
}

// The six-way weights of a light arriving from direction d, a unit vector toward the light in the
// sprite's frame. Each is a squared component on one side: wA = (+X, +Y, -Z) for lightA's maps and
// wB = (-X, -Y, +Z) for lightB's. Squared components always sum to 1, so a light between two axes
// interpolates their maps and gains no energy, where plain components would brighten a diagonal by
// up to sqrt(3).
void SixWayWeights(float3 d, out float3 wA, out float3 wB)
{
    float3 positive = max(d, float3(0.0));
    float3 negative = max(-d, float3(0.0));
    wA = float3(positive.x, positive.y, negative.z);
    wB = float3(negative.x, negative.y, positive.z);
    wA *= wA;
    wB *= wB;
}

// Adds a light of the given radiance arriving from d (unit, sprite frame) to a gather.
//
// directionality blends the weights from isotropic (0: one sixth on every axis) to SixWayWeights
// (1); the weights sum to 1 either way. A light inside the medium it lights passes a value below 1,
// so a light at the medium's centre lights every face alike.
void AddSixWayLight(inout SpriteLightGather gather, float3 d, float3 radiance,
                    float directionality)
{
    float3 wA;
    float3 wB;
    SixWayWeights(d, wA, wB);
    float t = saturate(directionality);
    wA = lerp(float3(1.0 / 6.0), wA, t);
    wB = lerp(float3(1.0 / 6.0), wB, t);
    float3 scaled = radiance / SpriteShadingPi;
    gather.In[0] += scaled * wA.x;
    gather.In[1] += scaled * wA.y;
    gather.In[2] += scaled * wA.z;
    gather.In[3] += scaled * wB.x;
    gather.In[4] += scaled * wB.y;
    gather.In[5] += scaled * wB.z;
}

// The lit radiance of a six-way texel: each map weighted by the light along its axis, plus the
// ambient times the six maps' mean. Both maps are premultiplied by coverage, so the result is too.
float3 ShadeSixWayLit(float3 lightA, float3 lightB, SpriteLightGather gather)
{
    float3 lit = lightA.x * gather.In[0] + lightA.y * gather.In[1] + lightA.z * gather.In[2] +
                 lightB.x * gather.In[3] + lightB.y * gather.In[4] + lightB.z * gather.In[5];
    float mean = (lightA.x + lightA.y + lightA.z + lightB.x + lightB.y + lightB.z) / 6.0;
    return lit + gather.Ambient * mean;
}

// ---------------------------------------------------------------------------------------------
// Temperature and emission
// ---------------------------------------------------------------------------------------------

// An emission ramp, laid out as the renderer's GpuEmissionRamp (std430, 144 bytes).
struct SpriteEmissionRamp
{
    // (r, g, b, t): linear HDR colour at normalised temperature t, ascending in t.
    float4 Keys[SpriteEmissionMaxKeys];
    // x the ignition threshold, y the key count; zw unused.
    float4 Params;
};

// A ramp with no keys, which emits nothing.
SpriteEmissionRamp SpriteEmissionRampNone()
{
    SpriteEmissionRamp ramp;
    for (uint i = 0; i < SpriteEmissionMaxKeys; ++i)
    {
        ramp.Keys[i] = float4(0.0);
    }
    ramp.Params = float4(0.0);
    return ramp;
}

// The normalised temperature a stored value stands for: stored² under the sqrt transfer (isSqrt),
// stored itself under the linear one.
float DecodeTemperature(float stored, bool isSqrt)
{
    return isSqrt ? stored * stored : stored;
}

// The temperature a texel shades at: its decoded value shifted by offset (a cooling puff reads
// cooler) and clamped to [0, 1].
float SpriteTemperature(float stored, bool isSqrt, float offset)
{
    return saturate(DecodeTemperature(stored, isSqrt) + offset);
}

// The ramp's colour at temperature t: piecewise linear between the keys, and held at the first or
// last key's colour outside them. A ramp with no keys is black.
float3 EvaluateEmissionRamp(SpriteEmissionRamp ramp, float t)
{
    uint count = min(uint(ramp.Params.y), SpriteEmissionMaxKeys);
    if (count == 0u)
    {
        return float3(0.0);
    }
    if (t <= ramp.Keys[0].w)
    {
        return ramp.Keys[0].rgb;
    }
    for (uint i = 1; i < count; ++i)
    {
        float4 upper = ramp.Keys[i];
        if (t <= upper.w)
        {
            float4 lower = ramp.Keys[i - 1];
            float span = max(upper.w - lower.w, 1e-6);
            return lerp(lower.rgb, upper.rgb, saturate((t - lower.w) / span));
        }
    }
    return ramp.Keys[count - 1].rgb;
}

// How far temperature t is past ignition, in [0, 1]: a ramp across a band of the given width
// centred on the threshold, saturate((t - ignition) / width + 0.5), and a hard step (1 at or above
// ignition) when width is 0. A fragment passes fwidth(t), which bounds the band to about a pixel:
// an analytic edge, so it holds with no anti-aliasing and stays 0 a pixel below ignition.
float IgnitionGate(float t, float ignition, float width)
{
    if (width <= 0.0)
    {
        return t >= ignition ? 1.0 : 0.0;
    }
    return saturate((t - ignition) / width + 0.5);
}

// The emitted radiance of a texel at temperature t, gated at the ramp's ignition across gateWidth,
// times coverage and intensity. Premultiplied, like the lit term.
float3 SixWayEmission(SpriteEmissionRamp ramp, float t, float gateWidth, float coverage,
                      float intensity)
{
    float gate = IgnitionGate(t, ramp.Params.x, gateWidth);
    return EvaluateEmissionRamp(ramp, t) * (gate * coverage * intensity);
}

// A shaded six-way texel.
struct SixWayShade
{
    // Premultiplied HDR colour in rgb, coverage in a, both times opacity.
    float4 Color;
    // The emitted radiance before opacity: the term meant to drive bloom.
    float3 Emission;
};

// Shades one six-way texel: the lit term times tint, plus the emission, both times opacity.
// lightA carries coverage in its alpha. t is the shading temperature (SpriteTemperature), and
// gateWidth the ignition gate's band (IgnitionGate).
SixWayShade ShadeSixWay(float4 lightA, float3 lightB, float t, float gateWidth,
                        SpriteLightGather gather, SpriteEmissionRamp ramp, float3 tint,
                        float opacity, float emissionIntensity)
{
    float coverage = lightA.a;
    float3 lit = ShadeSixWayLit(lightA.rgb, lightB, gather) * tint;
    float3 emission = SixWayEmission(ramp, t, gateWidth, coverage, emissionIntensity);
    SixWayShade shade;
    shade.Color = float4((lit + emission) * opacity, coverage * opacity);
    shade.Emission = emission;
    return shade;
}

// ---------------------------------------------------------------------------------------------
// The unlit path
// ---------------------------------------------------------------------------------------------

// The alpha conventions of an unlit sheet, matching FlipbookAlpha.
static const uint SpriteAlphaPremultiplied = 0;
static const uint SpriteAlphaCoverage = 1;
static const uint SpriteAlphaLuminance = 2;
static const uint SpriteAlphaOpaque = 3;

static const float3 SpriteLumaWeights = float3(0.2126, 0.7152, 0.0722);

// How much of a pixel a shaded sprite or particle owns, for a temporal resolve's reactive mask: its
// coverage, or its luminance where an additive source carries light with little coverage.
float SpriteReactiveCoverage(float4 premultiplied)
{
    return saturate(max(premultiplied.a, dot(premultiplied.rgb, SpriteLumaWeights)));
}

// Shades one texel of an unlit sheet: its alpha convention brought to premultiplied colour and
// coverage, then tint and opacity applied.
float4 ShadeUnlitSprite(float4 texel, uint alphaMode, float3 tint, float opacity)
{
    float3 color = texel.rgb;
    float coverage = texel.a;
    if (alphaMode == SpriteAlphaCoverage)
    {
        color *= coverage;
    }
    else if (alphaMode == SpriteAlphaLuminance)
    {
        coverage = saturate(dot(color, SpriteLumaWeights));
    }
    else if (alphaMode == SpriteAlphaOpaque)
    {
        coverage = 1.0;
    }
    return float4(color * tint * opacity, coverage * opacity);
}

// ---------------------------------------------------------------------------------------------
// Blending between frames
// ---------------------------------------------------------------------------------------------

// The motion offset an 8-bit-centred stored pair stands for: ((255·stored − 128) / 127) · scale,
// so a stored 128 decodes to exactly zero. In units of the full frame extent, v down.
float2 DecodeMotion(float2 stored, float scale)
{
    return ((255.0 * stored - 128.0) / 127.0) * scale;
}

// Where two consecutive frames sit in an atlas, and how far playback is between them.
struct SpriteFramePair
{
    // Frame N's atlas rectangle (u0, v0, u1, v1), inset half a texel from its tile's edges.
    float4 RectN;
    // Frame N+1's atlas rectangle, inset the same way.
    float4 RectN1;
    // One tile's extent in atlas UV.
    float2 TileSize;
    // Frame N's extent as a fraction of the full frame (1 when the flipbook records none).
    float ScaleN;
    // Frame N+1's extent as a fraction of the full frame.
    float ScaleN1;
    // How far playback is from frame N toward frame N+1, in [0, 1).
    float Fraction;
};

// The extent of the drawn quad between the two frames, as a fraction of the full frame. A quad
// sized by it about the frame centre keeps a cloud whose frames differ in extent at a steady
// world size.
float SpriteFrameExtent(SpriteFramePair pair)
{
    return lerp(pair.ScaleN, pair.ScaleN1, pair.Fraction);
}

// The two atlas taps that blend into one fragment, with their sampling gradients.
struct SpriteFrameTaps
{
    // Frame N's atlas coordinate, inside RectN.
    float2 UvN;
    // Frame N+1's atlas coordinate, inside RectN1.
    float2 UvN1;
    // d(UvN)/dx.
    float2 DxN;
    // d(UvN)/dy.
    float2 DyN;
    // d(UvN1)/dx.
    float2 DxN1;
    // d(UvN1)/dy.
    float2 DyN1;
    // The blend weight of frame N+1.
    float Fraction;
};

// The taps for a fragment at quad coordinate q (frame UV of the drawn quad: u right, v down,
// (0.5, 0.5) its centre), warped along the motion of each frame.
//
// The blend works in extent-normalised space, the space motion is measured in: the fragment sits at
// w = (q − ½)·SpriteFrameExtent. Frame N is read where the feature now at w was in frame N,
// (w − motionN·f) / ScaleN + ½, and frame N+1 where it will be,
// (w + motionN1·(1 − f)) / ScaleN1 + ½; each is mapped into its tile and clamped into its inset rectangle, so a warp never reads a
// neighbouring tile. motionN is frame N's own motion and motionN1 frame N+1's, each read at that
// frame's unwarped coordinate (these taps with zero motion). With zero motion the blend is a plain
// crossfade, and at f = 0 the result is frame N's own sample. A tile maps edge to edge across the
// quad, so q = 0 and q = 1 read its outermost texel centres through the clamp.
//
// dqdx and dqdy are q's screen derivatives (zero outside a fragment stage reads the top level).
// The gradients come from the unwarped coordinate, so a sharp edge in the motion field does not
// spike the mip level.
SpriteFrameTaps SpriteFrameTapsAt(SpriteFramePair pair, float2 q, float2 dqdx, float2 dqdy,
                                  float2 motionN, float2 motionN1)
{
    float extent = SpriteFrameExtent(pair);
    float2 w = (q - 0.5) * extent;
    float2 frameN = (w - motionN * pair.Fraction) / pair.ScaleN;
    float2 frameN1 = (w + motionN1 * (1.0 - pair.Fraction)) / pair.ScaleN1;
    float2 centreN = (pair.RectN.xy + pair.RectN.zw) * 0.5;
    float2 centreN1 = (pair.RectN1.xy + pair.RectN1.zw) * 0.5;

    SpriteFrameTaps taps;
    taps.UvN = clamp(centreN + frameN * pair.TileSize, pair.RectN.xy, pair.RectN.zw);
    taps.UvN1 = clamp(centreN1 + frameN1 * pair.TileSize, pair.RectN1.xy, pair.RectN1.zw);
    float2 gradientN = pair.TileSize * (extent / pair.ScaleN);
    float2 gradientN1 = pair.TileSize * (extent / pair.ScaleN1);
    taps.DxN = dqdx * gradientN;
    taps.DyN = dqdy * gradientN;
    taps.DxN1 = dqdx * gradientN1;
    taps.DyN1 = dqdy * gradientN1;
    taps.Fraction = pair.Fraction;
    return taps;
}

// Samples one atlas texture at a pair of taps and blends them: lerp(N, N+1, Fraction). The blend
// acts on the stored values, as the texture filter does, so shading runs once on the result.
float4 SampleFrameBlended(Texture2D atlas, SamplerState atlasSampler, SpriteFrameTaps taps)
{
    float4 n = atlas.SampleGrad(atlasSampler, taps.UvN, taps.DxN, taps.DyN);
    if (taps.Fraction <= 0.0)
    {
        return n;
    }
    float4 n1 = atlas.SampleGrad(atlasSampler, taps.UvN1, taps.DxN1, taps.DyN1);
    return lerp(n, n1, taps.Fraction);
}

// ---------------------------------------------------------------------------------------------
// Soft edges
// ---------------------------------------------------------------------------------------------

// The opacity left where a medium meets opaque geometry: saturate(gap / distance), with gap the
// opaque scene's view depth minus the fragment's. 1 when distance is 0 or less (no fade).
float SoftEdgeFade(float gap, float distance)
{
    if (distance <= 0.0)
    {
        return 1.0;
    }
    return saturate(gap / distance);
}
