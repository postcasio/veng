// Screen-space reflection trace: for each reflective fragment, reflects the view
// vector about the g-buffer normal and marches the ray through the depth buffer to
// find the first surface it crosses, then samples the lit scene color there. Writes
// the reflected radiance (rgb) and a confidence/mask (a) to a reflection target the
// SSR composite folds back into the scene.
//
// The march is a fixed-step linear walk in view space with a binary-search
// refinement once a crossing is bracketed: simple and robust, current-frame depth,
// no acceleration structure. (A min-Z hi-Z traversal replaces the inner loop as an
// optimization.) The trace runs at the reflection target's resolution (Extent), which
// may be below the render resolution; the g-buffer it reads is full-resolution and is
// sampled at the plain logical UV (SSR disables the dynamic-resolution sub-rect).
//
// View space is the well-behaved domain for the ray: the camera sits at the origin,
// the view vector to a fragment is its view-space position, and the depth comparison
// is a single Z test. World position is reconstructed via InvViewProj and carried
// into view space by View, so no separate inverse-projection is needed.
#include "view_constants.slang"

[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];

struct PushConstants
{
    uint SceneColorTexture;   // lit scene color (the radiance the ray samples)
    uint DepthTexture;        // depth attachment
    uint NormalTexture;       // G1 world-space normal
    uint OrmTexture;          // G2 packed occlusion/roughness/metallic
    uint HiZTexture;          // min-Z depth pyramid (empty-space skipping)
    uint Sampler;             // shared linear sampler
    uint ViewConstantsIndex;  // current frame-in-flight's view-constants region
    uint HiZLevels;           // min-Z pyramid mip count
    uint2 Extent;             // reflection target pixel dimensions
    float MaxDistance;        // view-space max ray length
    float Thickness;          // view-space depth thickness accepted as a hit
    float MaxRoughness;       // surfaces rougher than this trace no ray
    uint MaxSteps;            // linear march step count
};

// Coarse min-Z level the empty-space skip queries: a 2^Level texel cell. The skip jump is
// capped at the cell's texel span so the ray never leaps past a nearer surface in a
// neighboring cell.
static const uint HiZSkipLevel = 5;

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

// Project a view-space position to a screen UV through the projection matrix.
float2 ViewToScreenUV(float4x4 proj, float3 viewPos)
{
    float4 clip = mul(proj, float4(viewPos, 1.0));
    float2 ndc = clip.xy / clip.w;
    return ndc * 0.5 + 0.5;
}

// Reconstruct the view-space position of a screen UV by unprojecting through
// InvViewProj to world space, then transforming into view space.
float3 ViewPositionAt(ViewConstants vc, float2 uv, float depth)
{
    float2 ndcXY = uv * 2.0 - 1.0;
    float4 worldH = mul(vc.InvViewProj, float4(ndcXY, depth, 1.0));
    float3 worldPos = worldH.xyz / worldH.w;
    return mul(vc.View, float4(worldPos, 1.0)).xyz;
}

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
    SamplerState samp = g_Samplers[NonUniformResourceIndex(g_PC.Sampler)];
    ViewConstants vc = LoadViewConstants(g_PC.ViewConstantsIndex);
    float2 uv = input.v_UV;

    float depth = g_Textures[NonUniformResourceIndex(g_PC.DepthTexture)].Sample(samp, uv).r;
    // The cleared background has no surface to reflect from.
    if (depth >= 1.0)
        return float4(0.0, 0.0, 0.0, 0.0);

    float roughness = g_Textures[NonUniformResourceIndex(g_PC.OrmTexture)].Sample(samp, uv).g;
    // Very rough surfaces reflect nothing coherent; skip the trace (the composite's
    // roughness LOD fades the rest out smoothly below this cutoff).
    if (roughness > g_PC.MaxRoughness)
        return float4(0.0, 0.0, 0.0, 0.0);

    float3 worldNormal = g_Textures[NonUniformResourceIndex(g_PC.NormalTexture)].Sample(samp, uv).xyz;
    float normalLength = length(worldNormal);
    if (normalLength < 1e-4)
        return float4(0.0, 0.0, 0.0, 0.0);
    float3 N = mul((float3x3)vc.View, worldNormal / normalLength);

    // The camera is at the view-space origin, so the fragment's view position is also
    // the view ray to it; reflect that about the surface normal.
    float3 viewPos = ViewPositionAt(vc, uv, depth);
    float3 R = normalize(reflect(normalize(viewPos), N));

    // Clamp the ray end so it never crosses the camera plane (view space looks down -Z,
    // so the surface is at z < 0): a ray angled back toward the camera (R.z > 0) would
    // project past w = 0 and produce a garbage screen position.
    const float NearClip = -0.01;
    float endDistance = g_PC.MaxDistance;
    if (R.z > 0.0)
    {
        endDistance = min(endDistance, (NearClip - viewPos.z) / R.z);
    }
    if (endDistance <= 0.0)
        return float4(0.0, 0.0, 0.0, 0.0);
    float3 endPos = viewPos + R * endDistance;

    // Project the ray's start and end into screen space (uv.xy + NDC depth). A straight
    // view-space segment projects to a straight segment in (uv, depth), so marching that
    // line in screen space — at roughly one texel per step — gives pixel-precise hits
    // with no view-space-step smear.
    float2 startUV = ViewToScreenUV(vc.Proj, viewPos);
    float4 endClip = mul(vc.Proj, float4(endPos, 1.0));
    float3 endNdc = endClip.xyz / endClip.w;
    float3 p0 = float3(startUV, depth);
    float3 p1 = float3(endNdc.xy * 0.5 + 0.5, endNdc.z);
    float3 delta = p1 - p0;

    // Step so the longer screen axis advances about one texel per step, capped at MaxSteps.
    float2 pixelSpan = abs(delta.xy) * float2(g_PC.Extent);
    float steps = clamp(max(pixelSpan.x, pixelSpan.y), 1.0, float(g_PC.MaxSteps));
    float3 stepVec = delta / steps;
    uint stepCount = uint(steps);

    float3 prevRay = p0;
    // The previous step's signed view-Z gap (stored - ray). Start negative: the ray begins
    // at its own surface and steps away in front of the scene, so the first real surface it
    // meets is a front→behind crossing, not an immediate hit.
    float prevDeltaZ = -1.0;
    bool hit = false;
    float2 hitUV = float2(0.0, 0.0);

    const uint skipLevel = min(HiZSkipLevel, max(g_PC.HiZLevels, 1u) - 1u);
    const float skipCap = float(1u << skipLevel);

    uint i = 1;
    while (i <= stepCount)
    {
        float3 ray = p0 + stepVec * float(i);

        // The ray left the screen — nothing more to march.
        if (any(ray.xy < 0.0) || any(ray.xy > 1.0))
            break;

        // Hi-Z empty-space skip: when the ray is heading deeper, look up the closest surface
        // over a coarse min-Z cell; if the ray is in front of all of it, jump toward that
        // surface in one stride (capped to the cell's span so it can't leap past a nearer
        // surface in a neighboring cell). Only the fine steps near a surface do the depth test.
        if (stepVec.z > 1e-7)
        {
            int2 baseCoord = clamp(int2(ray.xy * float2(g_PC.Extent)), int2(0, 0),
                                   int2(g_PC.Extent) - 1);
            int2 cellCoord = baseCoord >> skipLevel;
            float cellMinZ =
                g_Textures[NonUniformResourceIndex(g_PC.HiZTexture)].Load(int3(cellCoord, int(skipLevel))).r;
            if (ray.z < cellMinZ)
            {
                float stepsToSurface = (cellMinZ - ray.z) / stepVec.z;
                uint jump = uint(clamp(floor(stepsToSurface), 1.0, skipCap));
                prevRay = ray;
                prevDeltaZ = -1.0;
                i += jump;
                continue;
            }
        }

        float sampleDepth = g_Textures[NonUniformResourceIndex(g_PC.DepthTexture)].Sample(samp, ray.xy).r;
        if (sampleDepth >= 1.0)
        {
            // No geometry here (background): the ray is in front of nothing, so reset the
            // crossing state rather than carrying a stale "behind" gap across the gap.
            prevRay = ray;
            prevDeltaZ = -1.0;
            ++i;
            continue;
        }

        // Compare in view space so the thickness test is a real distance, not an NDC-depth
        // delta (which is wildly non-linear). deltaZ > 0 means the ray is behind the stored
        // surface. A hit is a genuine crossing — in front last step, behind this step —
        // within Thickness, so the ray registers one hit at the surface rather than smearing
        // the whole band behind an object's silhouette.
        float storedZ = ViewPositionAt(vc, ray.xy, sampleDepth).z;
        float rayZ = ViewPositionAt(vc, ray.xy, ray.z).z;
        float deltaZ = storedZ - rayZ;

        if (prevDeltaZ < 0.0 && deltaZ >= 0.0 && deltaZ < g_PC.Thickness)
        {
            // Bracketed a crossing between prevRay and ray; bisect in screen space to refine.
            float3 lo = prevRay;
            float3 hi = ray;
            [unroll]
            for (uint b = 0; b < 5; ++b)
            {
                float3 mid = (lo + hi) * 0.5;
                float midDepth = g_Textures[NonUniformResourceIndex(g_PC.DepthTexture)].Sample(samp, mid.xy).r;
                float midStored = ViewPositionAt(vc, mid.xy, midDepth).z;
                float midRay = ViewPositionAt(vc, mid.xy, mid.z).z;
                if (midStored - midRay > 0.0)
                    hi = mid;
                else
                    lo = mid;
            }
            hitUV = hi.xy;
            hit = true;
            break;
        }

        prevRay = ray;
        prevDeltaZ = deltaZ;
        ++i;
    }

    if (!hit)
        return float4(0.0, 0.0, 0.0, 0.0);

    float3 reflected = g_Textures[NonUniformResourceIndex(g_PC.SceneColorTexture)].Sample(samp, hitUV).rgb;

    // Fade the contribution as the hit nears the screen border (the reflected detail
    // runs off-screen there) so reflections vanish smoothly rather than clipping.
    float2 edge = smoothstep(0.0, 0.15, hitUV) * (1.0 - smoothstep(0.85, 1.0, hitUV));
    float confidence = edge.x * edge.y;

    return float4(reflected, confidence);
}
