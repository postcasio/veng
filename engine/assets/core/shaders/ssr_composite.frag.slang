// SSR composite: folds the traced reflection back into the lit scene color, weighted
// by a Fresnel term (so grazing angles and metals reflect more) and the reflection's
// confidence mask, selecting the blur mip by surface roughness (a rougher surface
// reads a coarser, blurrier mip). Writes the combined linear HDR the bloom/tonemap
// tail consumes — SSR slots in where TAA does, so the rest of the tail is unchanged.
//
// SSR forces full render resolution, so g-buffer reads use the plain logical UV.
#include "view_constants.slang"

[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];

struct PushConstants
{
    uint SceneColorTexture;   // lit scene color (the base the reflection adds onto)
    uint ReflectionTexture;   // the SSR reflection mip chain (sampled by roughness LOD)
    uint AlbedoTexture;       // G0 base color (metallic F0 tint)
    uint NormalTexture;       // G1 world-space normal
    uint OrmTexture;          // G2 packed occlusion/roughness/metallic
    uint DepthTexture;        // depth attachment (world-position reconstruction)
    uint Sampler;             // shared linear sampler (g-buffer reads)
    uint ReflectionSampler;   // trilinear sampler over the reflection mip chain
    uint ViewConstantsIndex;  // current frame-in-flight's view-constants region
    float Intensity;          // reflection mix scale
    uint MipCount;            // reflection mip levels (roughness → LOD)
    uint Pad0;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

float3 FresnelSchlick(float cosTheta, float3 f0)
{
    return f0 + (1.0 - f0) * pow(saturate(1.0 - cosTheta), 5.0);
}

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
    SamplerState samp = g_Samplers[NonUniformResourceIndex(g_PC.Sampler)];
    SamplerState reflSamp = g_Samplers[NonUniformResourceIndex(g_PC.ReflectionSampler)];
    float2 uv = input.v_UV;

    float3 scene = g_Textures[NonUniformResourceIndex(g_PC.SceneColorTexture)].Sample(samp, uv).rgb;

    float depth = g_Textures[NonUniformResourceIndex(g_PC.DepthTexture)].Sample(samp, uv).r;
    if (depth >= 1.0)
        return float4(scene, 1.0);

    float4 orm = g_Textures[NonUniformResourceIndex(g_PC.OrmTexture)].Sample(samp, uv);
    float roughness = clamp(orm.g, 0.04, 1.0);
    float metallic = orm.b;

    float3 albedo = g_Textures[NonUniformResourceIndex(g_PC.AlbedoTexture)].Sample(samp, uv).rgb;
    float3 worldNormal = g_Textures[NonUniformResourceIndex(g_PC.NormalTexture)].Sample(samp, uv).xyz;
    float normalLength = length(worldNormal);
    float3 N = normalLength > 1e-4 ? worldNormal / normalLength : float3(0.0, 0.0, 1.0);

    ViewConstants vc = LoadViewConstants(g_PC.ViewConstantsIndex);
    float2 ndcXY = uv * 2.0 - 1.0;
    float4 worldH = mul(vc.InvViewProj, float4(ndcXY, depth, 1.0));
    float3 worldPos = worldH.xyz / worldH.w;
    float3 V = normalize(vc.CameraPosition.xyz - worldPos);

    // A rougher surface reads a coarser (blurrier) reflection mip.
    float lod = roughness * float(max(g_PC.MipCount, 1u) - 1u);
    float4 reflection =
        g_Textures[NonUniformResourceIndex(g_PC.ReflectionTexture)].SampleLevel(reflSamp, uv, lod);

    // Metallic workflow base reflectance: dielectrics reflect a flat 0.04, metals tint
    // the reflection by albedo.
    float3 f0 = lerp(float3(0.04), albedo, metallic);
    float3 fresnel = FresnelSchlick(max(dot(N, V), 0.0), f0);

    float3 contribution = reflection.rgb * fresnel * reflection.a * g_PC.Intensity;
    return float4(scene + contribution, 1.0);
}
