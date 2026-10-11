// SSR composite: folds the traced reflection back into the lit scene color, weighted by a
// Fresnel term (so grazing angles and metals reflect more), and writes the combined linear HDR
// the bloom/tonemap tail consumes — SSR slots in where TAA does, so the rest of the tail is
// unchanged. The reflection chain is premultiplied by the trace's confidence, so its color is
// added as it stands: a level that averaged few hits is dim by exactly that share.
//
// A rough surface reflects a cone, not a ray, and the composite blurs by that cone's footprint
// rather than by a fraction of the screen:
//
//  - The cone's half-angle is where the GGX half-vector distribution (width alpha = roughness^2)
//    falls to half its peak, tan(theta_h) = alpha * sqrt((sqrt2 - 1) / (1 - sqrt2 * alpha^2)),
//    doubled for the reflected direction. Past alpha^2 = 1/sqrt2 the lobe never halves, and the
//    cone is held at its widest.
//  - Its diameter at the hit is 2 * hitDistance * tan(theta_r), a world size. The reflected hit
//    is seen along the unfolded ray, cameraDistance + hitDistance from the camera, so that is the
//    distance the footprint is projected from — a contact reflection stays sharp and a far one
//    blurs toward the cone's own angle.
//  - The projection turns it into SSR texels last, through the view's projection and the chain's
//    own level-0 height, so a different render or SSR resolution changes how finely the blur is
//    sampled and not how wide it is.
//  - Level L of the chain, read with the B-spline below, has a kernel whose full width at half
//    maximum is about ChainFwhmTexels * 2^L level-0 texels, so the level matching a footprint is
//    log2(footprint / ChainFwhmTexels), clamped to the chain.
//
// The hit distance is read at the pixel from the finest level holding enough confidence: a pixel
// whose own ray missed takes its neighbourhood's mean from a coarser level, so a rough reflection
// blurs past the edge of what it reflects.
//
// Each coarse level is reconstructed with a cubic B-spline built from four bilinear taps, so it
// magnifies smoothly instead of drawing its texel grid, and two adjacent levels are blended by the
// LOD's fraction so the blur grows without popping.
//
// The composite runs at full render resolution (SSR disables the dynamic-resolution sub-rect), and
// the chain may be at a lower resolution than the target. Every surface it reads — the pixel's own
// and each chain tap's — is the one ssr_surface.slang takes on the unjittered ray, exactly as the
// trace took it, so under a temporal resolve the reflection and the scene it lands on both stand at
// unjittered positions. How level 0 is read depends on that ratio:
//
//  - At one composite pixel per SSR texel, level 0 is read bilinearly, which lands on the pixel's
//    own texel, so a mirror is exactly as sharp as the trace.
//  - Coarser, level 0 is magnified, and a bilinear read would draw the SSR grid as blocks and
//    smear a reflection across every silhouette. It is instead a joint-bilateral read of the 3x3
//    texels nearest the pixel: each tap's weight is the cubic B-spline's, times how well the
//    surface its ray left from agrees with the pixel's own. The B-spline's fourth tap per axis
//    carries at most 1/48 of the weight; it is folded out of the far tap, so the window moves to
//    the next texel without a jump. Between ratios 1 and 2 the weights blend from bilinear to the
//    B-spline, so the read is continuous in the ratio.
//  - A tap's surface is the one the trace took for that texel. It agrees in position when it lies near the pixel's tangent plane, within about an
//    SSR texel's width there, and in orientation when its normal is within about 25 degrees of the
//    pixel's, falling to nothing by 45; a background tap traced nothing and never agrees. The
//    weights are renormalized over the agreeing taps, so a reflection keeps its full strength up to
//    a silhouette and adds nothing past it, and confidence, carried in alpha, fades the same way.
//    A pixel no tap agrees with (a feature thinner than a texel) reflects nothing rather than its
//    neighbour's reflection.
#include "ssr_surface.slang"

[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];

struct PushConstants
{
    uint SceneColorTexture;   // lit scene color (the base the reflection adds onto)
    uint ReflectionTexture;   // the SSR reflection chain, premultiplied by confidence
    uint HitDistanceTexture;  // the SSR hit-distance chain, premultiplied by confidence
    uint AlbedoTexture;       // G0 base color (metallic F0 tint)
    uint NormalTexture;       // G1 world-space normal
    uint OrmTexture;          // G2 packed occlusion/roughness/metallic
    uint DepthTexture;        // depth attachment (position reconstruction)
    uint Sampler;             // shared linear sampler (the scene colour read)
    uint ReflectionSampler;   // trilinear clamp-to-edge sampler over the chains
    uint ViewConstantsIndex;  // current frame-in-flight's view-constants region
    float Intensity;          // reflection mix scale
    float MaxRoughness;       // the trace's roughness cutoff, which the reflection fades out toward
    uint MipCount;            // chain levels
    uint Pad0;
    uint Pad1;
    uint Pad2;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

// The full width at half maximum, in level-0 texels per 2^L, of chain level L read through the
// B-spline: the downsample kernels' variances (1.75 source texels squared a level) summed down
// the chain plus the B-spline's own (1/3 texel squared), about 0.92 * 4^L, give a sigma of about
// 0.96 * 2^L.
static const float ChainFwhmTexels = 2.25;

// sqrt(2) - 1: where the GGX distribution falls to half its peak, in tan^2(theta_h) / alpha^2.
static const float HalfMaximumScale = 0.41421356;

// The widest reflected cone half-angle the footprint is taken at (radians); a lobe this wide
// reads the chain's coarsest level anyway.
static const float MaxConeHalfAngle = 1.4;

// The least confidence a level must hold for its mean hit distance to be read.
static const float DistanceConfidenceFloor = 0.02;

// The share of [0, MaxRoughness] over which the reflection fades out before the cutoff.
static const float RoughnessFadeBand = 0.2;

// A tap's distance from the pixel's tangent plane, in SSR texel widths at the pixel, up to which
// it agrees fully, and from which it agrees not at all.
static const float PlaneAgreementFull = 0.5;
static const float PlaneAgreementNone = 2.0;

// The cosine between a tap's normal and the pixel's above which it agrees fully (about 25
// degrees), and below which it agrees not at all (about 45 degrees).
static const float NormalAgreementFull = 0.9;
static const float NormalAgreementNone = 0.7;

// The least agreeing weight the joint read renormalizes to; below it the reflection fades out.
static const float AgreementFloor = 1e-3;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

float3 FresnelSchlick(float cosTheta, float3 f0)
{
    return f0 + (1.0 - f0) * pow(saturate(1.0 - cosTheta), 5.0);
}

// The half-angle of the reflected direction's cone for a perceptual roughness (see the header).
float ReflectedConeHalfAngle(float roughness)
{
    float alpha = roughness * roughness;
    float alpha2 = alpha * alpha;
    float falloff = 1.0 - sqrt(2.0) * alpha2;
    if (falloff <= 1e-4)
        return MaxConeHalfAngle;
    float tanHalfVector = sqrt(HalfMaximumScale * alpha2 / falloff);
    return min(2.0 * atan(tanHalfVector), MaxConeHalfAngle);
}

// A cubic B-spline sample of one level from four bilinear taps: the B-spline's 4x4 weights are
// separable, and each adjacent pair along an axis folds into one bilinear tap placed by the pair's
// weight ratio.
float4 SampleBSpline(Texture2D chain, SamplerState samp, float2 uv, uint level)
{
    uint width;
    uint height;
    uint levels;
    chain.GetDimensions(level, width, height, levels);
    float2 size = float2(width, height);

    float2 coord = uv * size - 0.5;
    float2 base = floor(coord);
    float2 f = coord - base;
    float2 f2 = f * f;
    float2 f3 = f2 * f;
    float2 w0 = (1.0 - 3.0 * f + 3.0 * f2 - f3) / 6.0;
    float2 w1 = (4.0 - 6.0 * f2 + 3.0 * f3) / 6.0;
    float2 w2 = (1.0 + 3.0 * f + 3.0 * f2 - 3.0 * f3) / 6.0;
    float2 w3 = f3 / 6.0;

    float2 g0 = w0 + w1;
    float2 g1 = w2 + w3;
    float2 uv0 = (base - 0.5 + w1 / g0) / size;
    float2 uv1 = (base + 1.5 + w3 / g1) / size;

    float4 t00 = chain.SampleLevel(samp, float2(uv0.x, uv0.y), float(level));
    float4 t10 = chain.SampleLevel(samp, float2(uv1.x, uv0.y), float(level));
    float4 t01 = chain.SampleLevel(samp, float2(uv0.x, uv1.y), float(level));
    float4 t11 = chain.SampleLevel(samp, float2(uv1.x, uv1.y), float(level));
    return g0.y * (g0.x * t00 + g1.x * t10) + g1.y * (g0.x * t01 + g1.x * t11);
}

// The cubic B-spline at distance d in [0, 2] texels.
float BSpline(float d)
{
    return d < 1.0 ? (4.0 - 6.0 * d * d + 3.0 * d * d * d) / 6.0 : (2.0 - d) * (2.0 - d) * (2.0 - d) / 6.0;
}

// One axis's weights over the texels nearest - 1, nearest and nearest + 1, for the pixel's offset
// t in [-0.5, 0.5] from the nearest texel: bilinear at spline 0, the cubic B-spline at spline 1
// with its fourth tap (a^3 / 6) folded out of the far one, so both windows agree where t crosses
// half a texel.
float3 AxisWeights(float t, float spline)
{
    float a = abs(t);
    float centre = lerp(1.0 - a, BSpline(a), spline);
    float nearTap = lerp(a, BSpline(1.0 - a), spline);
    float farTap = spline * ((1.0 - a) * (1.0 - a) * (1.0 - a) - a * a * a) / 6.0;
    float3 weights = t >= 0.0 ? float3(farTap, centre, nearTap) : float3(nearTap, centre, farTap);
    return weights / (centre + nearTap + farTap);
}

// The surface a composite pixel reflects from, which a chain tap must agree with.
struct Surface
{
    float3 Position;  // camera-relative position
    float3 Normal;    // world-space unit normal
    float Tolerance;  // one level-0 texel's width at the pixel, in world units
};

// How well the surface level-0 texel `texel`'s ray left from agrees with the pixel's, in [0, 1].
// The surface is taken at the texel's centre exactly as the trace took it there.
float Agreement(ViewConstants vc, Surface pixel, int2 texel, float2 chainSize)
{
    SsrSurface tap = SsrSurfaceAt(vc, g_Textures[NonUniformResourceIndex(g_PC.DepthTexture)],
                                  g_Textures[NonUniformResourceIndex(g_PC.NormalTexture)],
                                  (float2(texel) + 0.5) / chainSize);
    if (!tap.Valid)
        return 0.0;

    float planeDistance = abs(dot(pixel.Normal, tap.Position - pixel.Position));
    float position =
        1.0 - smoothstep(PlaneAgreementFull, PlaneAgreementNone, planeDistance / pixel.Tolerance);
    float orientation = smoothstep(NormalAgreementNone, NormalAgreementFull, dot(pixel.Normal, tap.Normal));
    return position * orientation;
}

// Level 0 magnified past the composite: the B-spline over the 3x3 texels nearest the pixel, each
// weighted by its surface's agreement with the pixel's and renormalized over the agreeing ones.
// `spline` per axis blends the weights from bilinear (at one pixel per texel) to the B-spline.
float4 SampleLevelZeroJoint(Texture2D chain, ViewConstants vc, Surface pixel, float2 uv, float2 spline)
{
    uint width;
    uint height;
    uint levels;
    chain.GetDimensions(0u, width, height, levels);
    float2 size = float2(width, height);
    float2 coord = uv * size - 0.5;
    float2 nearest = round(coord);
    float3 wx = AxisWeights(coord.x - nearest.x, spline.x);
    float3 wy = AxisWeights(coord.y - nearest.y, spline.y);

    float4 sum = float4(0.0, 0.0, 0.0, 0.0);
    float total = 0.0;
    [unroll]
    for (int j = 0; j < 3; ++j)
    {
        [unroll]
        for (int i = 0; i < 3; ++i)
        {
            int2 texel = clamp(int2(nearest) + int2(i - 1, j - 1), int2(0, 0), int2(size) - 1);
            float weight = wx[i] * wy[j] * Agreement(vc, pixel, texel, size);
            sum += chain.Load(int3(texel, 0)) * weight;
            total += weight;
        }
    }
    return sum / max(total, AgreementFloor);
}

// One chain level: level 0 bilinearly at one composite pixel per texel, through the joint read
// when magnified; every coarser one through the B-spline.
float4 SampleLevel(Texture2D chain, SamplerState samp, ViewConstants vc, Surface pixel, float2 uv,
                   float2 spline, uint level)
{
    if (level != 0u)
        return SampleBSpline(chain, samp, uv, level);
    if (all(spline <= 0.0))
        return chain.SampleLevel(samp, uv, 0.0);
    return SampleLevelZeroJoint(chain, vc, pixel, uv, spline);
}

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
    SamplerState samp = g_Samplers[NonUniformResourceIndex(g_PC.Sampler)];
    SamplerState reflSamp = g_Samplers[NonUniformResourceIndex(g_PC.ReflectionSampler)];
    float2 uv = input.v_UV;

    // Every read names its level: most sit past an early return, where implicit derivatives are
    // undefined.
    float3 scene =
        g_Textures[NonUniformResourceIndex(g_PC.SceneColorTexture)].SampleLevel(samp, uv, 0.0).rgb;

    ViewConstants vc = LoadViewConstants(g_PC.ViewConstantsIndex);
    Texture2D depthTexture = g_Textures[NonUniformResourceIndex(g_PC.DepthTexture)];
    SsrSurface surface =
        SsrSurfaceAt(vc, depthTexture, g_Textures[NonUniformResourceIndex(g_PC.NormalTexture)], uv);
    // The cleared background has nothing to reflect into.
    if (!surface.Valid)
        return float4(scene, 1.0);

    float4 orm = g_Textures[NonUniformResourceIndex(g_PC.OrmTexture)].Load(int3(surface.Texel, 0));
    float roughness = saturate(orm.g);
    if (roughness >= g_PC.MaxRoughness)
        return float4(scene, 1.0);
    float roughnessFade =
        smoothstep(0.0, 1.0, (g_PC.MaxRoughness - roughness) / (RoughnessFadeBand * g_PC.MaxRoughness));
    float metallic = orm.b;

    Texture2D reflectionChain = g_Textures[NonUniformResourceIndex(g_PC.ReflectionTexture)];
    Texture2D distanceChain = g_Textures[NonUniformResourceIndex(g_PC.HitDistanceTexture)];
    uint mipCount = max(g_PC.MipCount, 1u);

    float hitDistance = -1.0;
    for (uint level = 0u; level < mipCount; ++level)
    {
        float confidence = reflectionChain.SampleLevel(reflSamp, uv, float(level)).a;
        if (confidence > DistanceConfidenceFloor)
        {
            hitDistance = distanceChain.SampleLevel(reflSamp, uv, float(level)).r / confidence;
            break;
        }
    }
    // Nothing around this pixel was hit at any level, so there is nothing to reflect.
    if (hitDistance < 0.0)
        return float4(scene, 1.0);

    // The camera-relative position, reconstructed without the camera's translation.
    float3 relative = surface.Position;
    float3 viewPos = mul((float3x3)vc.View, relative);
    float cameraDistance = max(length(relative), 1e-6);
    float3 N = surface.Normal;
    float3 V = -relative / cameraDistance;

    // The cone's footprint at the hit, projected to SSR texels from where the hit appears.
    float footprint = 2.0 * hitDistance * tan(ReflectedConeHalfAngle(roughness));
    uint chainWidth;
    uint chainHeight;
    uint chainLevels;
    reflectionChain.GetDimensions(0u, chainWidth, chainHeight, chainLevels);
    float texelsPerUnit = abs(vc.Proj[1][1]) * 0.5 * float(chainHeight);
    bool orthographic = vc.Proj[3][3] > 0.5;
    if (!orthographic)
    {
        texelsPerUnit /= max(abs(viewPos.z), 1e-6);
    }
    // Under perspective the footprint is seen from the unfolded hit, further than the surface.
    float footprintTexels = footprint * texelsPerUnit;
    if (!orthographic)
    {
        footprintTexels *= cameraDistance / (cameraDistance + hitDistance);
    }
    float lod = clamp(log2(max(footprintTexels / ChainFwhmTexels, 1.0)), 0.0, float(mipCount - 1u));

    // How many composite pixels one level-0 texel spans per axis, beyond the first: 0 reads level 0
    // bilinearly, 1 or more through the full B-spline.
    uint compositeWidth;
    uint compositeHeight;
    uint compositeLevels;
    depthTexture.GetDimensions(0u, compositeWidth, compositeHeight, compositeLevels);
    float2 spline = saturate(float2(compositeWidth, compositeHeight) / float2(chainWidth, chainHeight) - 1.0);
    Surface pixel = { relative, N, 1.0 / texelsPerUnit };

    uint fine = uint(lod);
    uint coarse = min(fine + 1u, mipCount - 1u);
    float blend = lod - float(fine);
    float4 reflection = SampleLevel(reflectionChain, reflSamp, vc, pixel, uv, spline, fine);
    if (blend > 0.0 && coarse != fine)
        reflection =
            lerp(reflection, SampleLevel(reflectionChain, reflSamp, vc, pixel, uv, spline, coarse), blend);

    // Metallic workflow base reflectance: dielectrics reflect a flat 0.04, metals tint
    // the reflection by albedo.
    float3 albedo = g_Textures[NonUniformResourceIndex(g_PC.AlbedoTexture)].Load(int3(surface.Texel, 0)).rgb;
    float3 f0 = lerp(float3(0.04), albedo, metallic);
    float3 fresnel = FresnelSchlick(max(dot(N, V), 0.0), f0);

    float3 contribution = reflection.rgb * fresnel * roughnessFade * g_PC.Intensity;
    return float4(scene + contribution, 1.0);
}
