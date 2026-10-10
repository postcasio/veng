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
//    log2(footprint / ChainFwhmTexels), clamped to the chain. Level 0 is read bilinearly, so a
//    mirror is as sharp as the trace.
//
// The hit distance is read at the pixel from the finest level holding enough confidence: a pixel
// whose own ray missed takes its neighbourhood's mean from a coarser level, so a rough reflection
// blurs past the edge of what it reflects.
//
// Each level is reconstructed with a cubic B-spline built from four bilinear taps, so a coarse
// level magnifies smoothly instead of drawing its texel grid, and two adjacent levels are blended
// by the LOD's fraction so the blur grows without popping.
//
// The composite runs at full render resolution: it reads the g-buffer at the plain logical UV
// (SSR disables the dynamic-resolution sub-rect), and the chain may be at a lower resolution than
// the target.
#include "view_constants.slang"

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
    uint Sampler;             // shared linear sampler (g-buffer reads)
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

// One chain level: level 0 bilinearly (the trace's own sharpness), every coarser one through the
// B-spline.
float4 SampleLevel(Texture2D chain, SamplerState samp, float2 uv, uint level)
{
    if (level == 0u)
        return chain.SampleLevel(samp, uv, 0.0);
    return SampleBSpline(chain, samp, uv, level);
}

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
    SamplerState samp = g_Samplers[NonUniformResourceIndex(g_PC.Sampler)];
    SamplerState reflSamp = g_Samplers[NonUniformResourceIndex(g_PC.ReflectionSampler)];
    float2 uv = input.v_UV;

    // Every read names its level: most sit past an early return, where implicit derivatives are
    // undefined and the anisotropic g-buffer sampler would blend texels across a silhouette.
    float3 scene =
        g_Textures[NonUniformResourceIndex(g_PC.SceneColorTexture)].SampleLevel(samp, uv, 0.0).rgb;

    float depth = g_Textures[NonUniformResourceIndex(g_PC.DepthTexture)].SampleLevel(samp, uv, 0.0).r;
    // Reverse-Z: the cleared background sits at depth 0.
    if (depth <= 0.0)
        return float4(scene, 1.0);

    float4 orm = g_Textures[NonUniformResourceIndex(g_PC.OrmTexture)].SampleLevel(samp, uv, 0.0);
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

    ViewConstants vc = LoadViewConstants(g_PC.ViewConstantsIndex);
    // The camera-relative position, reconstructed without the camera's translation.
    float2 ndcXY = uv * 2.0 - 1.0;
    float4 relativeH = mul(vc.InvViewRotProj, float4(ndcXY, depth, 1.0));
    float3 relative = relativeH.xyz / relativeH.w;
    float3 viewPos = mul((float3x3)vc.View, relative);
    float cameraDistance = max(length(relative), 1e-6);

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
        float apparentDepth = max(abs(viewPos.z), 1e-6) * (cameraDistance + hitDistance) / cameraDistance;
        texelsPerUnit /= apparentDepth;
    }
    float footprintTexels = footprint * texelsPerUnit;
    float lod = clamp(log2(max(footprintTexels / ChainFwhmTexels, 1.0)), 0.0, float(mipCount - 1u));

    uint fine = uint(lod);
    uint coarse = min(fine + 1u, mipCount - 1u);
    float blend = lod - float(fine);
    float4 reflection = SampleLevel(reflectionChain, reflSamp, uv, fine);
    if (blend > 0.0 && coarse != fine)
        reflection = lerp(reflection, SampleLevel(reflectionChain, reflSamp, uv, coarse), blend);

    float3 worldNormal =
        g_Textures[NonUniformResourceIndex(g_PC.NormalTexture)].SampleLevel(samp, uv, 0.0).xyz;
    float normalLength = length(worldNormal);
    float3 N = normalLength > 1e-4 ? worldNormal / normalLength : float3(0.0, 0.0, 1.0);
    float3 V = -relative / cameraDistance;

    // Metallic workflow base reflectance: dielectrics reflect a flat 0.04, metals tint
    // the reflection by albedo.
    float3 albedo = g_Textures[NonUniformResourceIndex(g_PC.AlbedoTexture)].SampleLevel(samp, uv, 0.0).rgb;
    float3 f0 = lerp(float3(0.04), albedo, metallic);
    float3 fresnel = FresnelSchlick(max(dot(N, V), 0.0), f0);

    float3 contribution = reflection.rgb * fresnel * roughnessFade * g_PC.Intensity;
    return float4(scene + contribution, 1.0);
}
