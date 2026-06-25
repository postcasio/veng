// Deferred lighting fragment stage: samples the g-buffer (G0 albedo, G1
// world-space normal, G2 packed occlusion/roughness/metallic/emissive, and the
// depth attachment) through the bindless set-0 array, reconstructs world position
// from depth, and evaluates a Cook-Torrance BRDF over a list of typed lights read
// from the per-frame light buffer. Writes linear HDR.
//
// VE_USE_SSAO selects a compile-time variant: when defined (by the
// deferred_lighting_ssao.frag wrapper), the screen-space AO target is sampled and
// folded into the ambient/occlusion term. SSAO is a compile variant, not a
// per-frame branch — the renderer selects the lighting pipeline by its AO setting.
//
// VE_DEBUG_CASCADES selects the DebugView::Cascades arm: the fragment is tinted by
// the cascade its view-space depth selects (the same selection the shadow sample
// uses) and written to the output unlit, so cascade selection is observable.
//
// Color space: G0 is an sRGB target, so the sample decodes albedo to linear; the
// HDR output is a linear float target, so the lit color stays linear (the tail
// tonemap/composite handles display encoding). World-space normals are sampled
// straight from G1 (a signed float target). World position is reconstructed from
// the depth attachment via ViewConstants.InvViewProj — one of the two depth targets
// read as textures in the engine — rather than stored in a fat position target.
#include "view_constants.slang"
#include "shadow.slang"

[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];
[[vk::binding(6, 0)]] ByteAddressBuffer g_Lights;

// Set 2: the image-based-lighting maps (off the bindless registry, a closed
// renderer-owned set). The radiance cube backs the skybox; irradiance is the diffuse
// term; the prefiltered cube + the BRDF LUT are the split-sum specular term. Always
// bound and valid; sampling is gated on the IblEnabled push flag.
[[vk::binding(0, 2)]] TextureCube g_Radiance;
[[vk::binding(1, 2)]] TextureCube g_Irradiance;
[[vk::binding(2, 2)]] TextureCube g_Prefilter;
[[vk::binding(3, 2)]] Texture2D g_BrdfLut;
[[vk::binding(4, 2)]] SamplerState g_IblSampler;

static const uint LightStride = 64;

// LightType, matching the Light builtin's enum (Directional/Point/Spot).
static const float LightTypeDirectional = 0.0;
static const float LightTypePoint = 1.0;
static const float LightTypeSpot = 2.0;

// One light, packed by the renderer into the ring-buffered light buffer. Position
// is the entity's world transform; the type rides DirectionType.w.
struct GpuLight
{
    float4 PositionRange;   // xyz world position, w range
    float4 DirectionType;   // xyz travel direction, w LightType
    float4 ColorIntensity;  // rgb linear color, a intensity
    float4 Cone;            // x cos(inner), y cos(outer), zw pad
};

struct PushConstants
{
    uint AlbedoTexture;       // G0 bindless index
    uint NormalTexture;       // G1 bindless index
    uint OrmTexture;          // G2 bindless index
    uint DepthTexture;        // depth bindless index
    uint Sampler;             // shared sampler index
    uint ViewConstantsIndex;  // current frame-in-flight's view-constants region
    uint LightBase;           // current frame's base index into the light buffer
    uint LightCount;          // live light count this frame
    uint IblEnabled;          // 1 = split-sum IBL ambient; 0 = flat ambient fallback
    uint SkyboxEnabled;       // 1 = fill cleared-depth background with the radiance cube
    uint PrefilterMipCount;   // prefiltered specular mip count (roughness -> LOD)
    float EnvIntensity;       // scales the IBL ambient + skybox radiance
#if VE_USE_SSAO
    uint SsaoTexture;         // screen-space AO bindless index (the SSAO variant only)
    uint Pad0;
    uint Pad1;
    uint Pad2;
#endif
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

static const float Pi = 3.14159265358979323846;

// A constant hemispheric ambient, multiplied by the surface's occlusion term so
// occlusion reads visibly. Sized larger than a flat fill so the occlusion channel
// (and Plan 05's SSAO, which folds into the same term) is perceptible.
static const float3 AmbientColor = float3(0.12, 0.13, 0.16);

// GGX / Trowbridge-Reitz normal distribution.
float DistributionGGX(float nDotH, float roughness)
{
    float a = roughness * roughness;
    float a2 = a * a;
    float d = nDotH * nDotH * (a2 - 1.0) + 1.0;
    return a2 / max(Pi * d * d, 1e-7);
}

// Smith geometry with the Schlick-GGX approximation, direct-lighting k.
float GeometrySchlickGGX(float nDotX, float roughness)
{
    float r = roughness + 1.0;
    float k = (r * r) / 8.0;
    return nDotX / (nDotX * (1.0 - k) + k);
}

float GeometrySmith(float nDotV, float nDotL, float roughness)
{
    return GeometrySchlickGGX(nDotV, roughness) * GeometrySchlickGGX(nDotL, roughness);
}

float3 FresnelSchlick(float cosTheta, float3 f0)
{
    return f0 + (1.0 - f0) * pow(saturate(1.0 - cosTheta), 5.0);
}

// Roughness-aware Fresnel for the IBL ambient term: a rough surface's grazing
// reflectance is bounded by its f0 rather than rising to white.
float3 FresnelSchlickRoughness(float cosTheta, float3 f0, float roughness)
{
    float3 fr = max(float3(1.0 - roughness), f0);
    return f0 + (fr - f0) * pow(saturate(1.0 - cosTheta), 5.0);
}

// The Cook-Torrance contribution of one light: the surface's specular + diffuse
// response to the incoming radiance from direction L, scaled by N·L. The caller
// supplies the per-light radiance (already distance/cone-attenuated). f0 is the
// surface's base reflectance, already metal-tinted.
float3 EvaluateLight(float3 N, float3 V, float3 L, float3 radiance,
                     float3 albedo, float roughness, float metallic, float3 f0)
{
    float3 H = normalize(V + L);

    float nDotL = max(dot(N, L), 0.0);
    float nDotV = max(dot(N, V), 1e-4);
    float nDotH = max(dot(N, H), 0.0);
    float vDotH = max(dot(V, H), 0.0);

    float ndf = DistributionGGX(nDotH, roughness);
    float g = GeometrySmith(nDotV, nDotL, roughness);
    float3 fresnel = FresnelSchlick(vDotH, f0);

    float3 specular = (ndf * g * fresnel) / max(4.0 * nDotV * nDotL, 1e-4);

    float3 kd = (1.0 - fresnel) * (1.0 - metallic);
    float3 diffuse = kd * albedo / Pi;

    return (diffuse + specular) * radiance * nDotL;
}

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
    SamplerState samp = g_Samplers[NonUniformResourceIndex(g_PC.Sampler)];

    ViewConstants vc = LoadViewConstants(g_PC.ViewConstantsIndex);

    // The g-buffer is rendered into the dynamic-resolution sub-rect, so remap the logical UV into
    // the valid region before sampling (identity at scale 1.0). NDC reconstruction below keeps the
    // raw logical UV — the view is logically full-screen; only the storage is a sub-rect.
    float2 sampleUV = ScaledSampleUV(input.v_UV, vc.RenderScaleUV.xy, vc.MaxValidUV.xy);

    float3 albedo = g_Textures[NonUniformResourceIndex(g_PC.AlbedoTexture)].Sample(samp, sampleUV).rgb;
    float3 normal = g_Textures[NonUniformResourceIndex(g_PC.NormalTexture)].Sample(samp, sampleUV).xyz;
    float4 orm    = g_Textures[NonUniformResourceIndex(g_PC.OrmTexture)].Sample(samp, sampleUV);
    float depth   = g_Textures[NonUniformResourceIndex(g_PC.DepthTexture)].Sample(samp, sampleUV).r;

    float occlusion = orm.r;
    float roughness = clamp(orm.g, 0.04, 1.0);
    float metallic  = orm.b;
    float emissive  = orm.a;

    float normalLength = length(normal);
    float3 N = normalLength > 1e-4 ? normal / normalLength : float3(0.0, 0.0, 0.0);

    // Reconstruct world position: form NDC from the screen UV + sampled depth,
    // transform by the inverse view-projection, and perspective-divide.
    float2 ndcXY = input.v_UV * 2.0 - 1.0;
    float4 clip = float4(ndcXY, depth, 1.0);
    float4 worldH = mul(vc.InvViewProj, clip);
    float3 worldPos = worldH.xyz / worldH.w;

    // The fragment's view-space depth (positive distance in front of the camera),
    // the cascade-selection key for both the shadow sample and the debug tint.
    float viewDepth = -mul(vc.View, float4(worldPos, 1.0)).z;

#if VE_DEBUG_CASCADES
    // The cascade debug arm: tint by the selected cascade, unlit, so cascade
    // selection (near → cascade 0, far → a higher cascade) is directly observable.
    // The background (cleared depth = 1.0) reads black so the receiver stands out.
    if (depth >= 1.0)
        return float4(0.0, 0.0, 0.0, 1.0);
    return float4(CascadeDebugTint(viewDepth), 1.0);
#endif

    // Skybox: the cleared-depth background (no geometry) reads the environment radiance
    // along the view ray. Geometry (depth < 1) falls through to the lit path below.
    if (g_PC.SkyboxEnabled != 0 && depth >= 1.0)
    {
        float3 skyDir = normalize(worldPos - vc.CameraPosition.xyz);
        float3 sky = g_Radiance.SampleLevel(g_IblSampler, skyDir, 0.0).rgb * g_PC.EnvIntensity;
        return float4(sky, 1.0);
    }

    float3 V = normalize(vc.CameraPosition.xyz - worldPos);

    // Metallic workflow: dielectrics reflect a flat 0.04, metals tint specular by
    // albedo; diffuse vanishes for metals.
    float3 f0 = lerp(float3(0.04), albedo, metallic);

    // Accumulate the Cook-Torrance response over every live light. Directional
    // lights have no attenuation; point/spot fall off with distance, and spot adds
    // a cone factor.
    float3 direct = float3(0.0);
    for (uint i = 0; i < g_PC.LightCount; ++i)
    {
        GpuLight light = g_Lights.Load<GpuLight>((g_PC.LightBase + i) * LightStride);

        float type = light.DirectionType.w;
        float3 radiance = light.ColorIntensity.rgb * light.ColorIntensity.a;

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
            L = dist > 1e-4 ? toLight / dist : float3(0.0, 0.0, 1.0);

            // Inverse-square falloff with a smooth cutoff at Range: physically
            // correct near the light, faded to zero by the range radius so the loop
            // bound stays meaningful.
            float range = max(light.PositionRange.w, 1e-4);
            float distanceFactor = 1.0 / max(dist * dist, 1e-4);
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
            }
        }

        // The directional light is shadowed by the cascaded shadow atlas; a point or
        // spot light is shadowed by its punctual atlas tile, gated by its shadow slot
        // (Cone.z, -1 = unshadowed). Both sample through hardware SampleCmp.
        float visibility;
        if (isDirectional)
            visibility = DirectionalShadowVisibility(worldPos, N, L, viewDepth);
        else
        {
            int slot = int(light.Cone.z);
            visibility = PunctualShadowVisibility(worldPos, N, L, slot);
        }

        direct += EvaluateLight(N, V, L, radiance * attenuation, albedo, roughness, metallic, f0) * visibility;
    }

    // The ambient occlusion factor. The SSAO variant samples the screen-space AO
    // target and combines it with the baked ORM.r occlusion by min — SSAO modulates
    // the ambient/indirect term only, never the direct Cook-Torrance light. The
    // non-SSAO variant uses the baked occlusion (× OcclusionStrength, already folded
    // into ORM.r by the geometry pass's material) alone.
#if VE_USE_SSAO
    float ssao = g_Textures[NonUniformResourceIndex(g_PC.SsaoTexture)].Sample(samp, sampleUV).r;
    float ambientOcclusion = min(occlusion, ssao);
#else
    float ambientOcclusion = occlusion;
#endif

    // Indirect (ambient) term. With an environment bound, the flat hemispheric fill is
    // replaced by split-sum IBL: diffuse irradiance + GGX-prefiltered specular weighted by
    // the BRDF LUT. Both are gated by the combined occlusion so the occlusion channel reads.
    float3 ambient;
    if (g_PC.IblEnabled != 0)
    {
        float nDotV = max(dot(N, V), 1e-4);
        float3 fresnel = FresnelSchlickRoughness(nDotV, f0, roughness);
        float3 kd = (1.0 - fresnel) * (1.0 - metallic);

        float3 irradiance = g_Irradiance.SampleLevel(g_IblSampler, N, 0.0).rgb;
        float3 diffuseIbl = kd * irradiance * albedo;

        float3 reflection = reflect(-V, N);
        float maxMip = float(max(g_PC.PrefilterMipCount, 1u) - 1u);
        float3 prefiltered =
            g_Prefilter.SampleLevel(g_IblSampler, reflection, roughness * maxMip).rgb;
        float2 brdf = g_BrdfLut.SampleLevel(g_IblSampler, float2(nDotV, roughness), 0.0).rg;
        float3 specularIbl = prefiltered * (fresnel * brdf.x + brdf.y);

        ambient = (diffuseIbl + specularIbl) * g_PC.EnvIntensity * ambientOcclusion;
    }
    else
    {
        ambient = AmbientColor * albedo * ambientOcclusion;
    }

    // Scalar emissive: the surface's own albedo scaled by the emissive strength.
    float3 emission = albedo * emissive;

    return float4(direct + ambient + emission, 1.0);
}
