// Deferred lighting fragment stage: samples the g-buffer (G0 albedo, G1
// world-space normal, G2 packed occlusion/roughness/metallic/emissive, and the
// depth attachment) through the bindless set-0 array, reconstructs world position
// from depth, and evaluates a Cook-Torrance BRDF over a single directional light
// read from the per-frame view-constants buffer. Writes linear HDR.
//
// Color space: G0 is an sRGB target, so the sample decodes albedo to linear; the
// HDR output is a linear float target, so the lit color stays linear (the tail
// tonemap/composite handles display encoding). World-space normals are sampled
// straight from G1 (a signed float target). World position is reconstructed from
// the depth attachment via ViewConstants.InvViewProj — the only depth target read
// as a texture in the engine — rather than stored in a fat position target.
[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];
[[vk::binding(5, 0)]] ByteAddressBuffer g_ViewConstants;

static const uint ViewConstantsStride = 256;

struct ViewConstants
{
    float4x4 InvViewProj;
    float4   CameraPosition;
    float4   LightDirection;
    float4   LightColor;
};

struct PushConstants
{
    uint AlbedoTexture;       // G0 bindless index
    uint NormalTexture;       // G1 bindless index
    uint OrmTexture;          // G2 bindless index
    uint DepthTexture;        // depth bindless index
    uint Sampler;             // shared sampler index
    uint ViewConstantsIndex;  // current frame-in-flight's view-constants region
    uint Pad0;
    uint Pad1;
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

    // The light direction is the direction the light travels; the surface faces
    // the light along -direction.
    float3 L = normalize(-vc.LightDirection.xyz);
    float3 H = normalize(V + L);
    float intensity = vc.LightColor.a;
    float3 radiance = vc.LightColor.rgb * intensity;

    float nDotL = max(dot(N, L), 0.0);
    float nDotV = max(dot(N, V), 1e-4);
    float nDotH = max(dot(N, H), 0.0);
    float vDotH = max(dot(V, H), 0.0);

    // Metallic workflow: dielectrics reflect a flat 0.04, metals tint specular by
    // albedo; diffuse vanishes for metals.
    float3 f0 = lerp(float3(0.04), albedo, metallic);

    float ndf = DistributionGGX(nDotH, roughness);
    float g = GeometrySmith(nDotV, nDotL, roughness);
    float3 fresnel = FresnelSchlick(vDotH, f0);

    float3 specular = (ndf * g * fresnel) / max(4.0 * nDotV * nDotL, 1e-4);

    float3 kd = (1.0 - fresnel) * (1.0 - metallic);
    float3 diffuse = kd * albedo / Pi;

    float3 direct = (diffuse + specular) * radiance * nDotL;

    // Ambient gated by the baked occlusion (× OcclusionStrength, already folded
    // into ORM.r by the geometry pass's material) so the occlusion channel reads.
    float3 ambient = AmbientColor * albedo * occlusion;

    // Scalar emissive: the surface's own albedo scaled by the emissive strength.
    float3 emission = albedo * emissive;

    return float4(direct + ambient + emission, 1.0);
}
