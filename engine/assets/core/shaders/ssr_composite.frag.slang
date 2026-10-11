// SSR composite: weights the traced reflection by a Fresnel term (so grazing angles and metals
// reflect more) and either folds it into the lit scene color, writing the combined linear HDR the
// bloom/tonemap tail consumes (fsMain), or, under a temporal resolve, writes the reflection term
// alone with the screen motion of the image it reflects, for the SSR temporal pass to accumulate
// and add after the resolve (fsTerm). The reflection chain is premultiplied by the trace's
// confidence, so its color is added as it stands: a level that averaged few hits is dim by exactly
// that share.
//
// A rough surface reflects a cone, not a ray, and the composite blurs by that cone's footprint
// rather than by a fraction of the screen:
//
//  - The cone's half-angle is where the GGX half-vector distribution (width alpha = roughness^2)
//    falls to half its peak, tan(theta_h) = alpha * sqrt((sqrt2 - 1) / (1 - sqrt2 * alpha^2)),
//    doubled for the reflected direction. Past alpha^2 = 1/sqrt2 the lobe never halves, and the
//    cone is held at its widest. The roughness is the one the pixel is shaded with, widened by its
//    normal's variation across the pixel (SsrRoughness), as is the cutoff's fade below.
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
// Each coarse level is reconstructed with the cubic B-spline over the 3x3 texels nearest the pixel,
// so it magnifies smoothly instead of drawing its texel grid, and two adjacent levels are blended by
// the LOD's fraction so the blur grows without popping. The read is joint-bilateral like level 0's
// (below), so a rough reflection does not blur across a silhouette. The surface chain carries, down
// the same blur, the mean and variance of the device depth of the surfaces each coarse texel's
// reflection averaged, and a texel agrees when that mean lies on the pixel's tangent plane and its
// spread is no more than the plane's own across the level's kernel; a texel holding any of a
// second surface spreads by the depth step between them. No normal is carried down, so only
// position is tested. The weights are renormalized over the agreeing texels, and each level
// replaces the one beneath it by the share of its kernel that agrees, so near a silhouette, where
// the coarse texels straddle it, the read leans on finer levels whose texels do not: it blurs less
// there rather than across it.
//
// The composite runs at full render resolution (SSR disables the dynamic-resolution sub-rect), and
// the chain may be at a lower resolution than the target. Every surface it reads — the pixel's own
// and each chain tap's — is the one ssr_surface.slang takes, exactly as the trace took it, in the
// rasterized (under a temporal resolve, jittered) frame. How level 0 is read depends on that ratio:
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
//  - A tap's surface is the one the trace took for that texel. It agrees in position when it lies
//    near the pixel's tangent plane, within about an SSR texel's width there, and in orientation
//    when its normal is within about 25 degrees of the pixel's, falling to nothing by 45; a
//    background tap traced nothing and never agrees. The
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
    uint SurfaceTexture;      // the SSR surface chain, premultiplied by coverage
    uint Pad0;
    uint Pad1;
    // fsTerm: the previous unjittered (projection x rotation-only view) applied to the camera's
    // move since then, (camera - previous camera, 0); the clip offset a camera-relative point takes
    // between the two frames beyond what PrevFromCurDirection carries.
    float4 PrevClipTranslation;
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

// The least coverage a coarse surface texel must carry to hold a surface at all.
static const float CoverageFloor = 1e-3;

// The share of a coarse level's kernel agreeing with the pixel's surface below which the read
// keeps the finer level beneath it, and above which it takes the coarse level whole.
static const float LevelTrustNone = 0.25;
static const float LevelTrustFull = 0.75;

// How many levels below the one the footprint asks for the read starts from, bounding its cost.
static const uint FallbackLevels = 3u;

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
    float3 Position;        // camera-relative position
    float3 Normal;          // world-space unit normal
    float Tolerance;        // one level-0 texel's width at the pixel, in world units
    float DepthGradient2;   // squared device-depth gradient of the pixel's plane, per level-0 texel
};

// The device depth at which the pixel's tangent plane crosses the ray through rasterized screen UV
// `rasterUV`, or the pixel's own depth where the ray runs along the plane.
float PlaneDepthAt(ViewConstants vc, float3 position, float3 normal, float depth, float2 rasterUV)
{
    float3 onRay = RelativePosition(vc, rasterUV, depth);
    float3 direction = RelativePosition(vc, rasterUV, depth * 0.5) - onRay;
    float facing = dot(normal, direction);
    if (abs(facing) < 1e-6 * length(direction))
        return depth;
    float3 crossing = onRay + direction * (dot(normal, position - onRay) / facing);
    float4 clip = mul(vc.Proj, float4(mul((float3x3)vc.View, crossing), 1.0));
    return clip.z / clip.w;
}

// The per-axis variance, in level-0 texels squared, of chain level L's blur kernel: the downsample's
// 1.75 source texels squared a level, summed down the chain.
float ChainKernelVariance(uint level)
{
    return 1.75 * (float(1u << (2u * level)) - 1.0) / 3.0;
}

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

// How well the surfaces coarse texel `texel` of `level` averaged agree with the pixel's, in [0, 1].
// Their mean, at the texel's centre, must lie on the pixel's tangent plane, and their spread about
// it must be no more than the plane's own across the level's kernel — a texel straddling a
// silhouette, however little of the other surface it holds, spreads by the depth step between
// them. Both are held to the level-0 tolerance at the pixel, since a plane's mean lies on the
// plane at any level. No normal is carried down the chain, so only position is tested.
float CoarseAgreement(ViewConstants vc, Surface pixel, int2 texel, uint level, float2 size)
{
    float4 surface =
        g_Textures[NonUniformResourceIndex(g_PC.SurfaceTexture)].Load(int3(texel, int(level)));
    if (surface.z < CoverageFloor)
        return 0.0;
    float2 texelUV = (float2(texel) + 0.5) / size;
    float mean = surface.x / surface.z;
    float3 position = RelativePosition(vc, texelUV, mean);
    float planeDistance = abs(dot(pixel.Normal, position - pixel.Position));

    float variance = max(surface.y / surface.z - mean * mean, 0.0);
    float excess = sqrt(max(variance - pixel.DepthGradient2 * ChainKernelVariance(level), 0.0));
    float spread = length(RelativePosition(vc, texelUV, min(mean + excess, 1.0)) - position);

    return (1.0 - smoothstep(PlaneAgreementFull, PlaneAgreementNone, planeDistance / pixel.Tolerance)) *
           (1.0 - smoothstep(PlaneAgreementFull, PlaneAgreementNone, spread / pixel.Tolerance));
}

// A coarse level: the B-spline over the 3x3 texels nearest the pixel, each weighted by how well
// the surfaces its blur averaged agree with the pixel's and renormalized over the agreeing ones.
// `agreed` is the share of the B-spline's weight that agrees.
float4 SampleCoarseJoint(Texture2D chain, ViewConstants vc, Surface pixel, float2 uv, uint level,
                         out float agreed)
{
    uint width;
    uint height;
    uint levels;
    chain.GetDimensions(level, width, height, levels);
    float2 size = float2(width, height);
    float2 coord = uv * size - 0.5;
    float2 nearest = round(coord);
    float3 wx = AxisWeights(coord.x - nearest.x, 1.0);
    float3 wy = AxisWeights(coord.y - nearest.y, 1.0);

    float4 sum = float4(0.0, 0.0, 0.0, 0.0);
    float total = 0.0;
    [unroll]
    for (int j = 0; j < 3; ++j)
    {
        [unroll]
        for (int i = 0; i < 3; ++i)
        {
            int2 texel = clamp(int2(nearest) + int2(i - 1, j - 1), int2(0, 0), int2(size) - 1);
            float weight = wx[i] * wy[j] * CoarseAgreement(vc, pixel, texel, level, size);
            sum += chain.Load(int3(texel, int(level))) * weight;
            total += weight;
        }
    }
    agreed = total;
    return sum / max(total, AgreementFloor);
}

// Level 0: bilinearly at one composite pixel per texel, through the joint read when magnified.
float4 SampleLevelZero(Texture2D chain, SamplerState samp, ViewConstants vc, Surface pixel, float2 uv,
                       float2 spline)
{
    if (all(spline <= 0.0))
        return chain.SampleLevel(samp, uv, 0.0);
    return SampleLevelZeroJoint(chain, vc, pixel, uv, spline);
}

// The reflection at `lod`. From a few levels below it upward, each coarser level replaces the
// read beneath it by how much of its kernel agrees with the pixel's surface, so where a coarse
// level's texels straddle a silhouette the read leans on the finer levels whose texels do not,
// and blurs less there rather than across it.
float4 SampleAtLod(Texture2D chain, SamplerState samp, ViewConstants vc, Surface pixel, float2 uv,
                   float2 spline, float lod, uint mipCount)
{
    uint fine = uint(lod);
    uint coarse = min(fine + 1u, mipCount - 1u);
    uint first = fine > FallbackLevels ? fine - FallbackLevels : 0u;
    float agreed;
    float4 value = first == 0u ? SampleLevelZero(chain, samp, vc, pixel, uv, spline)
                               : SampleCoarseJoint(chain, vc, pixel, uv, first, agreed);
    for (uint level = first + 1u; level <= fine; ++level)
    {
        float4 coarser = SampleCoarseJoint(chain, vc, pixel, uv, level, agreed);
        value = lerp(value, coarser, smoothstep(LevelTrustNone, LevelTrustFull, agreed));
    }
    float blend = lod - float(fine);
    if (blend > 0.0 && coarse != fine)
    {
        float4 coarser = SampleCoarseJoint(chain, vc, pixel, uv, coarse, agreed);
        value = lerp(value, coarser, blend * smoothstep(LevelTrustNone, LevelTrustFull, agreed));
    }
    return value;
}

// The reflection a composite pixel adds.
struct Reflected
{
    float3 Contribution;     // the radiance the reflection adds to the pixel
    float3 VirtualPosition;  // camera-relative position of the reflected image: the hit, unfolded
                             // through the surface along the pixel's ray
    bool Reflects;           // a reflection was evaluated: a surface below the cutoff with a hit near it
};

Reflected EvaluateReflection(ViewConstants vc, float2 uv)
{
    Reflected result;
    result.Contribution = float3(0.0, 0.0, 0.0);
    result.VirtualPosition = float3(0.0, 0.0, 0.0);
    result.Reflects = false;

    SamplerState reflSamp = g_Samplers[NonUniformResourceIndex(g_PC.ReflectionSampler)];
    Texture2D depthTexture = g_Textures[NonUniformResourceIndex(g_PC.DepthTexture)];
    Texture2D normalTexture = g_Textures[NonUniformResourceIndex(g_PC.NormalTexture)];
    SsrSurface surface = SsrSurfaceAt(vc, depthTexture, normalTexture, uv);
    // The cleared background has nothing to reflect into.
    if (!surface.Valid)
        return result;

    // Every read names its level: most sit past an early return, where implicit derivatives are
    // undefined.
    Texture2D ormTexture = g_Textures[NonUniformResourceIndex(g_PC.OrmTexture)];
    float roughness =
        saturate(SsrRoughness(vc, depthTexture, normalTexture, ormTexture, surface.Texel));
    if (roughness >= g_PC.MaxRoughness)
        return result;
    float roughnessFade =
        smoothstep(0.0, 1.0, (g_PC.MaxRoughness - roughness) / (RoughnessFadeBand * g_PC.MaxRoughness));
    float metallic = ormTexture.Load(int3(surface.Texel, 0)).b;

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
        return result;

    // The camera-relative position, reconstructed without the camera's translation.
    float3 relative = surface.Position;
    float3 viewPos = mul((float3x3)vc.View, relative);
    float cameraDistance = max(length(relative), 1e-6);
    float3 N = surface.Normal;
    // The pixel's ray: from the camera under perspective, down the view axis under an orthographic
    // projection.
    bool orthographic = vc.Proj[3][3] > 0.5;
    float3 rayDirection =
        orthographic ? mul(float3(0.0, 0.0, -1.0), (float3x3)vc.View) : relative / cameraDistance;
    float3 V = -rayDirection;

    // The cone's footprint at the hit, projected to SSR texels from where the hit appears.
    float footprint = 2.0 * hitDistance * tan(ReflectedConeHalfAngle(roughness));
    uint chainWidth;
    uint chainHeight;
    uint chainLevels;
    reflectionChain.GetDimensions(0u, chainWidth, chainHeight, chainLevels);
    float texelsPerUnit = abs(vc.Proj[1][1]) * 0.5 * float(chainHeight);
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
    // The pixel plane's device-depth gradient, one level-0 texel along each axis.
    float4 pixelClip = mul(vc.Proj, float4(viewPos, 1.0));
    float pixelDepth = pixelClip.z / pixelClip.w;
    float2 chainTexel = 1.0 / float2(chainWidth, chainHeight);
    float gradientX =
        PlaneDepthAt(vc, relative, N, pixelDepth, uv + float2(chainTexel.x, 0.0)) - pixelDepth;
    float gradientY =
        PlaneDepthAt(vc, relative, N, pixelDepth, uv + float2(0.0, chainTexel.y)) - pixelDepth;
    Surface pixel = { relative, N, 1.0 / texelsPerUnit, gradientX * gradientX + gradientY * gradientY };

    float4 reflection = SampleAtLod(reflectionChain, reflSamp, vc, pixel, uv, spline, lod, mipCount);

    // Metallic workflow base reflectance: dielectrics reflect a flat 0.04, metals tint
    // the reflection by albedo.
    float3 albedo = g_Textures[NonUniformResourceIndex(g_PC.AlbedoTexture)].Load(int3(surface.Texel, 0)).rgb;
    float3 f0 = lerp(float3(0.04), albedo, metallic);
    float3 fresnel = FresnelSchlick(max(dot(N, V), 0.0), f0);

    // The reflected image stands where the reflected ray, unfolded through the surface, ends: along
    // the pixel's own ray, the hit distance past the surface. A plane mirror's image of a point is
    // exactly there, wherever the camera stands.
    result.Contribution = reflection.rgb * fresnel * roughnessFade * g_PC.Intensity;
    result.VirtualPosition = relative + rayDirection * hitDistance;
    result.Reflects = true;
    return result;
}

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
    SamplerState samp = g_Samplers[NonUniformResourceIndex(g_PC.Sampler)];
    float3 scene = g_Textures[NonUniformResourceIndex(g_PC.SceneColorTexture)]
                       .SampleLevel(samp, input.v_UV, 0.0)
                       .rgb;
    Reflected reflected = EvaluateReflection(LoadViewConstants(g_PC.ViewConstantsIndex), input.v_UV);
    return float4(scene + reflected.Contribution, 1.0);
}

// The motion marking a reflected image the previous frame cannot have seen: it lands the history
// read off-screen, which rejects it.
static const float2 UnseenMotion = float2(4.0, 4.0);

// The unjittered screen motion (current UV minus previous UV) of a camera-relative point that does
// not move in the world. The current position is the jittered projection with its jitter taken
// back out; the previous one is the same point's direction reprojected through
// PrevFromCurDirection, offset by the camera's move, so no world-space translation enters it.
float2 PointMotion(ViewConstants vc, float3 relative)
{
    float4 current = mul(vc.Proj, float4(mul((float3x3)vc.View, relative), 1.0));
    current.xy -= vc.JitterNdc.xy * current.w;
    float4 previous = mul(vc.PrevFromCurDirection, current) + g_PC.PrevClipTranslation;
    if (current.w <= 1e-6 || previous.w <= 1e-6)
        return UnseenMotion;
    float2 currentUV = (current.xy / current.w) * 0.5 + 0.5;
    float2 previousUV = (previous.xy / previous.w) * 0.5 + 0.5;
    return currentUV - previousUV;
}

struct TermOutput
{
    float4 Term : SV_Target0;    // the radiance the reflection adds; alpha 1 where one was evaluated
    float4 Motion : SV_Target1;  // the reflected image's unjittered screen motion since last frame,
                                 // and its view depth
};

// Under a temporal resolve: the reflection term alone, and how far the image it reflects moved on
// screen, for the SSR temporal pass to accumulate. The reflected scene is taken as still, so the
// motion is the camera's alone; a moving reflected object is left to the pass's validation.
[shader("fragment")]
TermOutput fsTerm(VSOutput input)
{
    ViewConstants vc = LoadViewConstants(g_PC.ViewConstantsIndex);
    Reflected reflected = EvaluateReflection(vc, input.v_UV);
    TermOutput output;
    output.Term = float4(reflected.Contribution, reflected.Reflects ? 1.0 : 0.0);
    output.Motion = float4(0.0, 0.0, 0.0, 0.0);
    if (reflected.Reflects)
    {
        float imageDepth = -mul((float3x3)vc.View, reflected.VirtualPosition).z;
        output.Motion = float4(PointMotion(vc, reflected.VirtualPosition), imageDepth, 0.0);
    }
    return output;
}
