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
// Color space: G0 is an sRGB target, so the sample decodes albedo to linear; the
// HDR output is a linear float target, so the lit color stays linear (the tail
// tonemap/composite handles display encoding). World-space normals are sampled
// straight from G1 (a signed float target). World position is reconstructed from
// the depth attachment via ViewConstants.InvViewProj — one of the two depth targets
// read as textures in the engine — rather than stored in a fat position target.
[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];
[[vk::binding(5, 0)]] ByteAddressBuffer g_ViewConstants;
[[vk::binding(6, 0)]] ByteAddressBuffer g_Lights;

static const uint ViewConstantsStride = 512;
static const uint LightStride = 64;

// LightType, matching the Light builtin's enum (Directional/Point/Spot).
static const float LightTypeDirectional = 0.0;
static const float LightTypePoint = 1.0;
static const float LightTypeSpot = 2.0;

struct ViewConstants
{
    float4x4 InvViewProj;
    float4   CameraPosition;
    float4x4 LightViewProj;  // directional light's world → light-clip transform
    float4   ShadowParams;   // x shadow-map handle, y sampler handle, z 1/resolution, w enabled (0/1)
    float4x4 View;           // world → view (the SSAO variant reconstructs view space)
    float4x4 Proj;           // view → clip
};

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

// Manual PCF visibility for the directional light. The shadow map stores the
// scene's depth from the light's orthographic view; a fragment is lit where its
// own light-space depth is no greater than the stored caster depth. The raw depth
// is sampled through an ordinary bindless sampler (not a hardware comparison
// sampler — a comparison sampler in set 0's shared bindless array mistranslates
// inside the Metal argument buffer on MoltenVK) and compared in-shader. A 3x3
// kernel of texel-offset samples is averaged to soften the edge, and a
// slope-scaled bias (larger as N·L falls toward grazing) curbs self-shadow acne.
float DirectionalShadowVisibility(float3 worldPos, float3 N, float3 L, ViewConstants vc)
{
    // ShadowParams.w gates the whole path: 0 when the shadow pass is compiled out,
    // so the term is fully lit and the stale slot is never sampled.
    if (vc.ShadowParams.w < 0.5)
        return 1.0;

    float4 lightClip = mul(vc.LightViewProj, float4(worldPos, 1.0));
    float3 lightNdc = lightClip.xyz / lightClip.w;

    // Outside the light's frustum (xy in NDC, depth in [0,1]) there is no shadow
    // data; treat it as lit rather than darkening unshadowed geometry.
    float2 shadowUV = lightNdc.xy * 0.5 + 0.5;
    if (shadowUV.x < 0.0 || shadowUV.x > 1.0 || shadowUV.y < 0.0 || shadowUV.y > 1.0
        || lightNdc.z < 0.0 || lightNdc.z > 1.0)
        return 1.0;

    uint shadowIndex = uint(vc.ShadowParams.x);
    uint samplerIndex = uint(vc.ShadowParams.y);
    float texel = vc.ShadowParams.z;

    Texture2D shadowMap = g_Textures[NonUniformResourceIndex(shadowIndex)];
    SamplerState shadowSamp = g_Samplers[NonUniformResourceIndex(samplerIndex)];

    // Slope-scaled bias: small head-on, growing as the surface turns away from the
    // light so a near-grazing texel does not shadow itself.
    float nDotL = saturate(dot(N, L));
    float bias = clamp(0.0015 * tan(acos(nDotL)), 0.0005, 0.005);
    float fragDepth = lightNdc.z - bias;

    float visibility = 0.0;
    for (int dy = -1; dy <= 1; ++dy)
        for (int dx = -1; dx <= 1; ++dx)
        {
            float2 offsetUV = shadowUV + float2(float(dx), float(dy)) * texel;
            float casterDepth = shadowMap.Sample(shadowSamp, offsetUV).r;
            visibility += fragDepth <= casterDepth ? 1.0 : 0.0;
        }

    return visibility / 9.0;
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

    float3 albedo = g_Textures[NonUniformResourceIndex(g_PC.AlbedoTexture)].Sample(samp, input.v_UV).rgb;
    float3 normal = g_Textures[NonUniformResourceIndex(g_PC.NormalTexture)].Sample(samp, input.v_UV).xyz;
    float4 orm    = g_Textures[NonUniformResourceIndex(g_PC.OrmTexture)].Sample(samp, input.v_UV);
    float depth   = g_Textures[NonUniformResourceIndex(g_PC.DepthTexture)].Sample(samp, input.v_UV).r;

    ViewConstants vc = g_ViewConstants.Load<ViewConstants>(g_PC.ViewConstantsIndex * ViewConstantsStride);

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

        // The directional light is shadowed by the shadow map (manual PCF); point
        // and spot lights are unshadowed (visibility 1.0).
        float visibility = isDirectional ? DirectionalShadowVisibility(worldPos, N, L, vc) : 1.0;

        direct += EvaluateLight(N, V, L, radiance * attenuation, albedo, roughness, metallic, f0) * visibility;
    }

    // The ambient occlusion factor. The SSAO variant samples the screen-space AO
    // target and combines it with the baked ORM.r occlusion by min — SSAO modulates
    // the ambient/indirect term only, never the direct Cook-Torrance light. The
    // non-SSAO variant uses the baked occlusion (× OcclusionStrength, already folded
    // into ORM.r by the geometry pass's material) alone.
#if VE_USE_SSAO
    float ssao = g_Textures[NonUniformResourceIndex(g_PC.SsaoTexture)].Sample(samp, input.v_UV).r;
    float ambientOcclusion = min(occlusion, ssao);
#else
    float ambientOcclusion = occlusion;
#endif

    // Ambient gated by the combined occlusion so the occlusion channel reads.
    float3 ambient = AmbientColor * albedo * ambientOcclusion;

    // Scalar emissive: the surface's own albedo scaled by the emissive strength.
    float3 emission = albedo * emissive;

    return float4(direct + ambient + emission, 1.0);
}
