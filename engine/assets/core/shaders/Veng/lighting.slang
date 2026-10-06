// The engine's shared surface-lighting core: the Cook-Torrance BRDF over the typed lights in the
// per-view light buffer (directional / point / spot analytically, Rect / Sphere / Polygon through
// Linearly Transformed Cosines), their shadowing, and the three-arm ambient term (split-sum IBL, SH
// skylight, flat floor). The deferred lighting pass evaluates it per g-buffer pixel and
// Veng/forward_lighting.slang per translucent fragment, so an opaque and a translucent surface with
// the same parameters receive the same light.
//
// The BRDF terms themselves (the GGX distribution, Smith geometry, Schlick Fresnel) live in the
// binding-free Veng/brdf.slang, shared with the split-sum bakes. The two entry points (the deferred
// pass and EvaluateForwardLighting) pass what they return through GuardLightingOutput, which keeps
// it inside the scene colour's half-float range.
//
// Everything a caller needs beyond the surface itself comes from the view block
// (view_constants.slang): the light-buffer region and count, the area-vertex region, the ambient
// arm and its parameters, and the LTC LUT handles. Results split diffuse from specular radiance —
// the deferred pass sums them, a forward material may weight them apart.
//
// The includer declares the set-0 bindless arrays g_Textures / g_Samplers and the view block
// (view_constants.slang) before including this, and chooses the shadow system's set through
// VE_SHADOW_SET (see shadow.slang). This file declares the image-based-lighting maps at set 4, and
// through Veng/light_buffer.slang the set-0 light and area-vertex buffers (bindings 6 and 7).
#include "../shadow.slang"
#include "Veng/brdf.slang"
#include "Veng/light_buffer.slang"
#include "Veng/sh.slang"
#include "Veng/light_flags.slang"
#include "Veng/ltc.slang"

// Set 4: the image-based-lighting maps (off the bindless registry, a closed renderer-owned set).
// Irradiance is the diffuse term; the prefiltered cube + the BRDF LUT are the split-sum specular
// term. Binding 0 (the radiance cube) is the skybox pass's and is unused here. Always bound and
// valid; sampling is gated on the view block's ambient arm.
[[vk::binding(1, 4)]] TextureCube g_Irradiance;
[[vk::binding(2, 4)]] TextureCube g_Prefilter;
[[vk::binding(3, 4)]] Texture2D g_BrdfLut;
[[vk::binding(4, 4)]] SamplerState g_IblSampler;

// The view block's ambient arm (LightState.w), matching the renderer's AmbientArm.
static const uint AmbientArmFlat = 0;
static const uint AmbientArmSkylight = 1;
static const uint AmbientArmIbl = 2;

// The largest radiance a lighting entry point returns, per channel. It sits below the RGBA16F scene
// colour's maximum (65504), so a lit value is never written as inf.
static const float LightingOutputMax = 6.0e4;

// Clamps lit radiance per channel to LightingOutputMax. The bound exists for overflow, not for the
// lobe: a full-strength GGX peak on a glossy metal under a bright light can exceed the half-float
// range, and an inf in the scene colour becomes a NaN wherever a later pass weights it (bloom's
// Karis average), which then spreads. Every value the target can represent passes unchanged.
//
// It bounds the radiance it is handed, not what a caller makes of it afterwards: a material that
// scales lit radiance up, or sums it with other radiance, passes its final colour through it again.
float3 GuardLightingOutput(float3 radiance)
{
    return min(radiance, float3(LightingOutputMax));
}

// The surface a lighting evaluation shades, in world space. Build it with MakeLightingSurface,
// which applies the roughness floor and derives the metal-tinted base reflectance.
struct LightingSurface
{
    float3 Position;         // world position
    float3 Normal;           // unit world-space shading normal
    float3 View;             // unit vector from the surface toward the camera
    float3 Albedo;           // linear base colour
    float  Roughness;        // perceptual roughness, floored at 0.04
    float  Metallic;         // [0,1]
    float3 F0;               // base reflectance: a flat 0.04 for dielectrics, albedo-tinted for metals
    float  ViewDepth;        // positive view-space distance in front of the camera (cascade select)
    bool   DeclinesShadowMaps; // take full visibility in place of every shadow-atlas lookup
};

// Diffuse and specular outgoing radiance, kept apart so a caller can weight them independently.
struct LightingResult
{
    float3 Diffuse;
    float3 Specular;
};

LightingSurface MakeLightingSurface(ViewConstants vc, float3 position, float3 normal,
                                    float3 view, float3 albedo, float roughness, float metallic,
                                    bool declinesShadowMaps)
{
    LightingSurface s;
    s.Position = position;
    s.Normal = normal;
    s.View = view;
    s.Albedo = albedo;
    s.Roughness = clamp(roughness, 0.04, 1.0);
    s.Metallic = metallic;
    // Metallic workflow: dielectrics reflect a flat 0.04, metals tint specular by albedo; diffuse
    // vanishes for metals.
    s.F0 = lerp(float3(0.04), albedo, metallic);
    s.ViewDepth = -mul(vc.View, float4(position, 1.0)).z;
    s.DeclinesShadowMaps = declinesShadowMaps;
    return s;
}

// The Cook-Torrance response of one light arriving from direction L with the given (already
// distance/cone-attenuated) radiance, scaled by N·L.
LightingResult EvaluateLight(LightingSurface s, float3 L, float3 radiance)
{
    float3 N = s.Normal;
    float3 V = s.View;
    float3 H = normalize(V + L);

    float nDotL = max(dot(N, L), 0.0);
    float nDotV = max(dot(N, V), 1e-4);
    float nDotH = max(dot(N, H), 0.0);
    float vDotH = max(dot(V, H), 0.0);

    // AccumulateLight calls this only with N·L > 0, so H is never opposite N and D stays finite.
    float ndf = BrdfDistributionGGX(N, H, nDotH, s.Roughness * s.Roughness);
    float g = BrdfGeometrySmith(nDotV, nDotL, s.Roughness);
    float3 fresnel = BrdfFresnelSchlick(vDotH, s.F0);

    float3 specular = (ndf * g * fresnel) / max(4.0 * nDotV * nDotL, 1e-4);

    float3 kd = (1.0 - fresnel) * (1.0 - s.Metallic);
    float3 diffuse = kd * s.Albedo / BrdfPi;

    LightingResult r;
    r.Diffuse = diffuse * radiance * nDotL;
    r.Specular = specular * radiance * nDotL;
    return r;
}

// ---- Area lights: Linearly Transformed Cosines (Heitz et al.) ----
//
// The geometry-agnostic core — the edge integrator, the horizon-clipped form factor, and the
// Sphere integrator — lives in Veng/ltc.slang, shared with the LTC test shader. This file supplies
// the pieces that name bindings: the LUT matrix sample and the Rect/Polygon vertex integrator.
static const float LtcLutSize = 64.0;
static const float LtcLutScale = (LtcLutSize - 1.0) / LtcLutSize;
static const float LtcLutBias = 0.5 / LtcLutSize;

// Sample the LTC LUTs by (roughness, sqrt(1 - N·V)) and reconstruct the inverse transform.
// t2 returns the BRDF magnitude (x) and Fresnel weight (y) the specular tint is split across.
float3x3 LtcMatrix(ViewConstants vc, float nDotV, float roughness, out float4 t2)
{
    SamplerState samp = g_Samplers[NonUniformResourceIndex(vc.LightLuts.z)];
    float2 uv = float2(roughness, sqrt(saturate(1.0 - nDotV)));
    uv = uv * LtcLutScale + LtcLutBias;
    float4 t1 = g_Textures[NonUniformResourceIndex(vc.LightLuts.x)].SampleLevel(samp, uv, 0.0);
    t2 = g_Textures[NonUniformResourceIndex(vc.LightLuts.y)].SampleLevel(samp, uv, 0.0);
    // The LUT packs glm column-major invM[col][row]: t1.y is row 2/col 0 and t1.z row 0/col 2,
    // so the row-major reconstruction places t1.z in the top row — swapping them transposes the
    // matrix and mirrors the specular lobe to the wrong side of the light.
    return float3x3(t1.x, 0.0, t1.z,
                    0.0, 1.0, 0.0,
                    t1.y, 0.0, t1.w);
}

// The LTC integral over a convex polygon read from the area-vertex buffer. Minv is the fitted
// matrix (specular) or identity (Lambert diffuse). areaNormal is the polygon's CCW front-face
// normal; a one-sided light contributes nothing to a point behind its plane.
float LtcPolygon(ViewConstants vc, float3 N, float3 V, float3 P, float3x3 Minv, uint base,
                 uint count, bool twoSided, float3 areaNormal)
{
    float3 T1 = normalize(V - N * dot(V, N));
    float3 T2 = cross(N, T1);
    // Slang's float3x3(a,b,c) fills rows, so this is already the world→(T1,T2,N) rotation.
    float3x3 M = mul(Minv, float3x3(T1, T2, N));

    float3 prev = normalize(mul(M, LoadAreaVertex(vc, base + count - 1u) - P));
    float3 vsum = float3(0.0);
    for (uint i = 0; i < count; ++i)
    {
        float3 cur = normalize(mul(M, LoadAreaVertex(vc, base + i) - P));
        vsum += LtcIntegrateEdgeVec(prev, cur);
        prev = cur;
    }

    float len = length(vsum);
    if (len < 1e-6)
    {
        return 0.0;
    }
    // The edge-vector z sums negative for a front face pointing at the shading point (the
    // Rect/Polygon CCW authoring convention), so the front-lit axis cosine is the negated z.
    float z = -vsum.z / len;
    bool behind = dot(P - LoadAreaVertex(vc, base), areaNormal) < 0.0;
    if (behind && !twoSided)
    {
        return 0.0;
    }
    if (behind)
    {
        z = -z;
    }
    return LtcClippedFormFactor(z, len);
}

// The full area-light contribution, before range attenuation and shadowing. Metals drop the
// diffuse arm; the specular arm is split across the Fresnel weights.
LightingResult EvaluateAreaLight(ViewConstants vc, LightingSurface s, GpuLight light)
{
    float3 N = s.Normal;
    float3 V = s.View;
    float3 P = s.Position;
    float nDotV = max(dot(N, V), 1e-4);
    float4 t2;
    float3x3 Minv = LtcMatrix(vc, nDotV, s.Roughness, t2);
    float3x3 ident = float3x3(1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0);

    float type = light.DirectionType.w;
    bool twoSided = LightIsTwoSided(light.Cone.w);

    LightingResult r;
    r.Diffuse = float3(0.0);
    r.Specular = float3(0.0);

    float specSum;
    float diffSum;
    if (type == LightTypeSphere)
    {
        float3 center = light.PositionRange.xyz;
        float radius = max(light.Area.x, 1e-4);
        specSum = LtcSphere(N, V, P, Minv, center, radius);
        diffSum = LtcSphere(N, V, P, ident, center, radius);
    }
    else
    {
        uint base = uint(light.Area.y);
        uint count = uint(light.Area.z);
        if (count < 3u)
        {
            return r;
        }
        specSum = LtcPolygon(vc, N, V, P, Minv, base, count, twoSided, light.AreaNormal.xyz);
        diffSum = LtcPolygon(vc, N, V, P, ident, base, count, twoSided, light.AreaNormal.xyz);
    }

    float3 specScale = s.F0 * t2.x + (1.0 - s.F0) * t2.y;
    float3 radiance = light.ColorIntensity.rgb * light.ColorIntensity.a;
    // diffSum/specSum are the normalized clamped-cosine form factor in [0, 1] (LtcClippedFormFactor),
    // so the reflected radiance is the emitter radiance times the BRDF terms directly — in the
    // point-light limit this matches EvaluateLight's diffuse albedo/Pi * radiance * nDotL.
    r.Diffuse = radiance * (s.Albedo * (1.0 - s.Metallic) * diffSum);
    r.Specular = radiance * (specSum * specScale);
    return r;
}

// The distance from P to the nearest point of an area light's emitter — zero on or inside it — which
// its range cutoff is measured from, so a long strip reaches as far from its ends as from its middle.
float AreaLightSurfaceDistance(ViewConstants vc, GpuLight light, float3 P)
{
    float3 center = light.PositionRange.xyz;
    if (light.DirectionType.w == LightTypeSphere)
    {
        return max(length(P - center) - light.Area.x, 0.0);
    }

    uint base = uint(light.Area.y);
    uint count = uint(light.Area.z);
    if (count < 3u)
    {
        return length(P - center);
    }

    // The polygon is convex and planar: P projects inside it when that projection sits on the same
    // side of every edge, and otherwise the nearest point lies on an edge.
    float3 n = light.AreaNormal.xyz;
    float planeDist = dot(P - LoadAreaVertex(vc, base), n);
    float3 Q = P - planeDist * n;
    bool anyInside = false;
    bool anyOutside = false;
    float edgeDist2 = 3.4e38;
    float3 prev = LoadAreaVertex(vc, base + count - 1u);
    for (uint i = 0u; i < count; ++i)
    {
        float3 cur = LoadAreaVertex(vc, base + i);
        float3 edge = cur - prev;
        float side = dot(cross(edge, Q - prev), n);
        anyInside = anyInside || side > 0.0;
        anyOutside = anyOutside || side < 0.0;
        float t = saturate(dot(P - prev, edge) / max(dot(edge, edge), 1e-8));
        float3 d = P - (prev + t * edge);
        edgeDist2 = min(edgeDist2, dot(d, d));
        prev = cur;
    }
    return (anyInside && anyOutside) ? sqrt(edgeDist2) : abs(planeDist);
}

// Area-light visibility through the punctual shadow atlas with PCSS softening. An area light is
// shadowed as a spot-style caster (its slot rides Cone.z, -1 = unshadowed); the penumbra widens
// with how large the shadow source (AreaNormal.w) subtends from this fragment, so a large light
// casts a soft shadow. AreaNormal.w is the shadow source radius, not the LTC emitter radius in
// Area.x — the two are the same authored size but only the shadow one is capped.
float AreaShadowVisibility(GpuLight light, float3 worldPos, float3 N)
{
    int slot = int(light.Cone.z);
    if (slot < 0)
    {
        return 1.0;
    }
    float3 toLight = light.PositionRange.xyz - worldPos;
    float3 L = normalize(toLight);
    return AreaShadowSample(slot, worldPos, N, L, light.AreaNormal.w, length(toLight));
}

// Adds one light's shadowed Cook-Torrance response at the surface to direct. Directional lights
// have no attenuation; point/spot fall off with distance, and spot adds a cone factor; area lights
// integrate analytically through LTC. A light whose contribution here is exactly zero — zero
// radiance, the pixel at or past its range or outside its cone, or the light behind the surface —
// returns before its shadow lookup and BRDF.
void AccumulateLight(ViewConstants vc, LightingSurface s, GpuLight light,
                     inout LightingResult direct)
{
    float3 N = s.Normal;
    float3 worldPos = s.Position;

    float type = light.DirectionType.w;
    float3 radiance = light.ColorIntensity.rgb * light.ColorIntensity.a;
    if (all(radiance == float3(0.0)))
    {
        return;
    }

    // Rect/Sphere/Polygon are area lights: shade analytically through the LTC path and fall
    // off with a smooth range cutoff only — the solid-angle integral already carries the
    // inverse-square, so no separate distance term. The cutoff is measured from the emitter's
    // surface, not its center, and is zero at the range, so the integral is skipped past it.
    // (Area shadowing is applied below.)
    if (type > LightTypeSpot + 0.5)
    {
        float dist = AreaLightSurfaceDistance(vc, light, worldPos);
        float range = max(light.PositionRange.w, 1e-4);
        if (dist >= range)
        {
            return;
        }
        LightingResult area = EvaluateAreaLight(vc, s, light);
        float rangeFactor = saturate(1.0 - pow(dist / range, 4.0));
        rangeFactor *= rangeFactor;
        // A near-parallel area light is shadowed by the cascade set fit to it — parallel
        // projection, fit to the camera view — using its travel direction, not its position.
        // Any other area light samples its punctual perspective tile.
        float areaVisibility;
        if (s.DeclinesShadowMaps)
        {
            areaVisibility = 1.0;
        }
        else if (LightIsAreaCascadeShadowed(light.Cone.w))
        {
            float3 sunL = normalize(-light.DirectionType.xyz);
            areaVisibility = DirectionalShadowVisibility(worldPos, N, sunL, s.ViewDepth,
                                                        LightCascadeSet(light.Cone.w));
        }
        else
        {
            areaVisibility = AreaShadowVisibility(light, worldPos, N);
        }
        direct.Diffuse += area.Diffuse * rangeFactor * areaVisibility;
        direct.Specular += area.Specular * rangeFactor * areaVisibility * light.Response.x;
        return;
    }

    float3 L;
    float attenuation = 1.0;
    bool isDirectional = type == LightTypeDirectional;
    if (isDirectional)
    {
        // The light direction is the direction the light travels; the surface
        // faces the light along -direction.
        L = normalize(-light.DirectionType.xyz);
    }
    else
    {
        // Point and spot are placed at PositionRange.xyz; L points from the
        // surface toward the light.
        float3 toLight = light.PositionRange.xyz - worldPos;
        float dist = length(toLight);
        float range = max(light.PositionRange.w, 1e-4);
        if (dist >= range)
        {
            return;
        }
        L = dist > 1e-4 ? toLight / dist : float3(0.0, 0.0, 1.0);

        // Inverse-square falloff with a smooth cutoff at Range: physically
        // correct near the light, faded to zero by the range radius so the loop
        // bound stays meaningful.
        //
        // The near field is bounded by the light's spherical source radius (Area.x):
        // the shading distance is clamped to be no less than it before the square, so a
        // real source size — not an epsilon guard — caps the response at contact. A zero
        // radius (the default) falls back to the 1e-4 floor and is bit-identical to a pure
        // inverse-square. A 1/(d^2 + 1) denominator is deliberately not used: the +1
        // perturbs the falloff at every distance, not only near the singularity, so it
        // cannot be read as absolute radiance under a fixed exposure. L still comes from the
        // true toLight — the clamp bounds the magnitude of the response, not its direction.
        float sourceRadius = light.Area.x;
        float distanceFactor = 1.0 / max(max(dist * dist, sourceRadius * sourceRadius), 1e-4);
        float rangeFactor = saturate(1.0 - pow(dist / range, 4.0));
        rangeFactor *= rangeFactor;
        attenuation = distanceFactor * rangeFactor;

        if (type == LightTypeSpot)
        {
            // Smoothstep the cone between cos(inner) (full) and cos(outer)
            // (zero); the surface-to-light direction is compared against the
            // light's travel direction.
            float cosAngle = dot(normalize(-light.DirectionType.xyz), L);
            float cosInner = light.Cone.x;
            float cosOuter = light.Cone.y;
            attenuation *= smoothstep(cosOuter, cosInner, cosAngle);
            if (attenuation <= 0.0)
            {
                return;
            }
        }
    }

    // EvaluateLight scales by the clamped N·L, so a light behind the surface adds exactly zero.
    if (dot(N, L) <= 0.0)
    {
        return;
    }

    // A directional light is shadowed by the cascade set fit to its direction, unless the
    // atlas had none left for it (the denied flag), in which case it shades unshadowed
    // rather than reading a cascade fit to some other light. A point or spot light is
    // shadowed by its punctual atlas tile, gated by its shadow slot (Cone.z, -1 =
    // unshadowed). Both sample through hardware SampleCmp.
    float visibility;
    if (s.DeclinesShadowMaps)
    {
        visibility = 1.0;
    }
    else if (isDirectional)
    {
        visibility = LightIsCascadeDenied(light.Cone.w)
            ? 1.0
            : DirectionalShadowVisibility(worldPos, N, L, s.ViewDepth,
                                          LightCascadeSet(light.Cone.w));
    }
    else
    {
        int slot = int(light.Cone.z);
        visibility = PunctualShadowVisibility(worldPos, N, L, slot);
    }

    LightingResult lit = EvaluateLight(s, L, radiance * attenuation);
    direct.Diffuse += lit.Diffuse * visibility;
    direct.Specular += lit.Specular * visibility * light.Response.x;
}

// The response of the surface to the lights of this view's light region that lightMask names (bit i
// the region's i-th light), shadowed. The lights are visited in index order, so a mask naming every
// light sums exactly what the unmasked loop does; a caller that knows a light cannot reach the
// surface — the deferred pass's per-tile cull — clears its bit and skips it outright.
LightingResult EvaluateDirectLightingMasked(ViewConstants vc, LightingSurface s, uint lightMask)
{
    LightingResult direct;
    direct.Diffuse = float3(0.0);
    direct.Specular = float3(0.0);

    uint remaining = lightMask & AllLightsMask(vc.LightState.y);
    while (remaining != 0u)
    {
        uint index = firstbitlow(remaining);
        remaining &= remaining - 1u;
        AccumulateLight(vc, s, LoadLight(vc, index), direct);
    }
    return direct;
}

// The Cook-Torrance response of the surface to every live light in this view's light region,
// shadowed.
LightingResult EvaluateDirectLighting(ViewConstants vc, LightingSurface s)
{
    return EvaluateDirectLightingMasked(vc, s, AllLightsMask(vc.LightState.y));
}

// The indirect (ambient) term, three-way on the view block's ambient arm. With an environment
// bound, split-sum IBL: diffuse irradiance + GGX-prefiltered specular weighted by the BRDF LUT.
// With no environment but a skylight, the diffuse-only SH ambient: the sky projected to order-2 SH,
// reconstructed per fragment and tinted by albedo. Otherwise the flat floor. Every arm is scaled
// by the ambient occlusion so the occlusion channel reads.
LightingResult EvaluateAmbientLighting(ViewConstants vc, LightingSurface s, float ambientOcclusion)
{
    float3 N = s.Normal;
    float3 V = s.View;

    LightingResult ambient;
    ambient.Specular = float3(0.0);

    uint arm = vc.LightState.w;
    if (arm == AmbientArmIbl)
    {
        float nDotV = max(dot(N, V), 1e-4);
        float3 fresnel = BrdfFresnelSchlickRoughness(nDotV, s.F0, s.Roughness);
        float3 kd = (1.0 - fresnel) * (1.0 - s.Metallic);

        float3 irradiance = g_Irradiance.SampleLevel(g_IblSampler, N, 0.0).rgb;
        float3 diffuseIbl = kd * irradiance * s.Albedo;

        float3 reflection = reflect(-V, N);
        float maxMip = float(max(vc.LightLuts.w, 1u) - 1u);
        float3 prefiltered =
            g_Prefilter.SampleLevel(g_IblSampler, reflection, s.Roughness * maxMip).rgb;
        float2 brdf = g_BrdfLut.SampleLevel(g_IblSampler, float2(nDotV, s.Roughness), 0.0).rg;
        float3 specularIbl = prefiltered * (fresnel * brdf.x + brdf.y);

        float scale = vc.AmbientFloor.w * ambientOcclusion;
        ambient.Diffuse = diffuseIbl * scale;
        ambient.Specular = specularIbl * scale;
    }
    else if (arm == AmbientArmSkylight)
    {
        // The cosine-convolved coefficients reconstruct cosine-weighted irradiance directly;
        // clamp the SH ringing below zero before it darkens the surface. The Lambertian
        // albedo/pi turns irradiance into outgoing radiance, matching the direct arm's
        // normalization (the IBL arm folds the same 1/pi into its irradiance convolution).
        float3 irradiance = max(ShEvalIrradiance(vc.SkyShCoeffs, N), float3(0.0));
        ambient.Diffuse =
            irradiance * (s.Albedo / BrdfPi) * vc.AmbientParams.x * ambientOcclusion;
    }
    else
    {
        ambient.Diffuse = vc.AmbientFloor.rgb * s.Albedo * ambientOcclusion;
    }
    return ambient;
}
