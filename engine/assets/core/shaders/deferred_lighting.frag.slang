// Deferred lighting fragment stage: samples the g-buffer (G0 albedo, G1
// world-space normal, G2 packed occlusion/roughness/metallic plus the surface-flags
// alpha, and the depth attachment) through the bindless set-0 array, reconstructs world position
// from depth, and evaluates the shared lighting core (Veng/lighting.slang) — a Cook-Torrance BRDF
// over the typed lights in the per-view light buffer, plus the ambient arm the view block names.
// Writes linear HDR.
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
// VE_DEBUG_IBL selects the DebugView::IblContribution arm: the fragment returns only
// its indirect (IBL/skylight/flat) ambient term (diffuse + specular IBL × occlusion),
// with the direct and emissive contributions dropped, so the environment lighting each
// pixel receives is directly observable.
//
// Color space: G0 is an sRGB target, so the sample decodes albedo to linear; the
// HDR output is a linear float target, so the lit color stays linear (the tail
// tonemap/composite handles display encoding). World-space normals are sampled
// straight from G1 (a signed float target). World position is reconstructed from
// the depth attachment via ViewConstants.InvViewProj — one of the two depth targets
// read as textures in the engine — rather than stored in a fat position target.
#include "view_constants.slang"
#include "Veng/surface_flags.slang"

[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];

// The shared lighting core: the BRDF, the light loop, shadowing, and the ambient arms, read from
// the per-view light state in the view block — the same evaluation a forward-lit translucent
// fragment runs (Veng/forward_lighting.slang). The shadow system sits at set 3 in this layout.
#include "Veng/lighting.slang"

struct PushConstants
{
    uint AlbedoTexture;       // G0 bindless index
    uint NormalTexture;       // G1 bindless index
    uint OrmTexture;          // G2 bindless index
    uint DepthTexture;        // depth bindless index
    uint EmissiveTexture;     // G4 emissive bindless index
    uint Sampler;             // shared sampler index
    uint ViewConstantsIndex;  // current view's view-constants region (carries the light state)
#if VE_USE_SSAO
    uint SsaoTexture;         // screen-space AO bindless index (the SSAO variant only)
#endif
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

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

    // Background (reverse-Z cleared depth = 0.0): no geometry was drawn here, so these pixels
    // belong to the skybox pass. Leave them black. The cleared g-buffer (occlusion 1, normal 0)
    // otherwise drives the IBL specular term — reflect(-V, 0) = -V samples the prefiltered
    // (low-resolution) environment along the view ray — which would paint a blurry skybox over the
    // background even when the skybox pass is disabled.
    if (depth <= 0.0)
    {
        return float4(0.0, 0.0, 0.0, 1.0);
    }

    float occlusion = orm.r;
    float roughness = orm.g;
    float metallic  = orm.b;
    // The surface-flags channel. A fragment whose material declines shadow-map reception is lit
    // exactly as any other, but every atlas lookup below resolves to full visibility: the world
    // position reconstructed for it is not where its geometry stands, so any silhouette found at
    // that coordinate belongs to unrelated geometry.
    bool declinesShadowMaps = GBufferDeclinesShadowMaps(orm.a);

    float normalLength = length(normal);
    float3 N = normalLength > 1e-4 ? normal / normalLength : float3(0.0, 0.0, 0.0);

    // Reconstruct world position: form NDC from the screen UV + sampled depth,
    // transform by the inverse view-projection, and perspective-divide.
    float2 ndcXY = input.v_UV * 2.0 - 1.0;
    float4 clip = float4(ndcXY, depth, 1.0);
    float4 worldH = mul(vc.InvViewProj, clip);
    float3 worldPos = worldH.xyz / worldH.w;

    float3 V = normalize(vc.CameraPosition.xyz - worldPos);
    LightingSurface surface =
        MakeLightingSurface(vc, worldPos, N, V, albedo, roughness, metallic, declinesShadowMaps);

#if VE_DEBUG_CASCADES
    // The cascade debug arm: tint by the selected cascade, unlit, so cascade
    // selection (near → cascade 0, far → a higher cascade) is directly observable.
    // Background pixels already returned black above.
    return float4(CascadeDebugTint(surface.ViewDepth), 1.0);
#endif

    LightingResult direct = EvaluateDirectLighting(vc, surface);

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

    LightingResult ambient = EvaluateAmbientLighting(vc, surface, ambientOcclusion);

#if VE_DEBUG_IBL
    // The IBL-contribution debug arm: the ambient term alone, unlit, so the environment
    // lighting each pixel receives (whichever ambient arm it took) is directly observable.
    // Background pixels already returned black above.
    return float4(ambient.Diffuse + ambient.Specular, 1.0);
#endif

    // G4 emissive: the linear HDR radiance the surface fragment authored, added into the
    // pixel's outgoing light. Placement here is equivalent to a post-lighting composite: the
    // skybox pass discards foreground pixels and geometry is foreground by definition, so
    // emissive on geometry survives the sky composite identically; the background (depth <= 0.0,
    // returned black above) carries none.
    float3 emissive = g_Textures[NonUniformResourceIndex(g_PC.EmissiveTexture)].Sample(samp, sampleUV).rgb;

    return float4(direct.Diffuse + direct.Specular + ambient.Diffuse + ambient.Specular + emissive,
                  1.0);
}
