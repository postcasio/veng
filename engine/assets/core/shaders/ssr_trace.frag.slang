// Screen-space reflection trace: for each reflective fragment, reflects the view
// vector about the g-buffer normal and marches the ray through the depth buffer to
// find the first surface it crosses, then samples the lit scene color there. Writes
// the reflected radiance (rgb) and a confidence/mask (a) to a reflection target the
// SSR composite folds back into the scene.
//
// The ray is a straight segment in view space, projected to a straight line in screen space
// (uv + NDC depth) and marched along that line at one texel per step, with a binary-search
// refinement once a crossing is bracketed. Two budgets bound the loop, both MaxSteps: depth tests
// and hi-Z probes. Empty space is skipped by a mip-climbing walk over the max-Z pyramid: a probe
// whose cell holds nothing in front of the ray advances the ray to where it leaves the cell (or
// reaches the cell's nearest depth) and climbs a level; one that finds something descends, and at
// level 0 the ray takes a depth test. A skip costs one probe, not the steps it crosses, and the
// depth-test stride widens only when the tests left could not otherwise reach the line's end. The
// trace runs at the reflection target's resolution (Extent), which may be below the render
// resolution; the g-buffer it reads is full-resolution and is sampled at the plain logical UV (SSR
// disables the dynamic-resolution sub-rect).
//
// A crossing — in front of the stored surface one test, behind it the next — is a hit when the gap
// behind it is under the larger of Thickness and StepToleranceScale times the depth the ray itself
// moved since the last test. A stride's depth travel grows with distance (one texel spans more
// depth further away), so a fixed thickness alone rejects every crossing at range and the ray
// tunnels; a gap wider than the ray's own travel means it slid behind a nearer surface, which the
// authored thickness alone governs.
//
// View positions are reconstructed camera-relative (InvViewRotProj, then the view rotation), so
// their precision does not depend on how far the camera is from the world origin; the camera sits
// at the view-space origin and the depth comparison is a single Z test.
#include "view_constants.slang"

[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];

struct PushConstants
{
    uint SceneColorTexture;   // lit scene color (the radiance the ray samples)
    uint DepthTexture;        // depth attachment
    uint NormalTexture;       // G1 world-space normal
    uint OrmTexture;          // G2 packed occlusion/roughness/metallic
    uint HiZTexture;          // max-Z depth pyramid (empty-space skipping)
    uint Sampler;             // shared linear sampler
    uint ViewConstantsIndex;  // current frame-in-flight's view-constants region
    uint HiZLevels;           // max-Z pyramid mip count
    uint2 Extent;             // reflection target pixel dimensions
    float MaxDistance;        // view-space max ray length
    float Thickness;          // view-space depth gap always accepted as a hit
    float MaxRoughness;       // surfaces rougher than this trace no ray
    uint MaxSteps;            // depth-test budget, and separately the hi-Z probe budget
};

// A crossing's gap may run to this multiple of the ray's own depth travel since the last test.
static const float StepToleranceScale = 1.5;

// The pyramid level the climb restarts from after a depth test: 2x2-texel cells.
static const uint HiZStartLevel = 1;

// How far past a cell boundary a skip lands, in steps (texels), so the next probe reads the next
// cell rather than the one just crossed.
static const float CellExitBias = 0.05;

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

// The view-space position of a screen UV at a device depth, unprojected through the camera-relative
// InvViewRotProj and rotated into view space, so no world-space translation enters it.
float3 ViewPositionAt(ViewConstants vc, float2 uv, float depth)
{
    float2 ndcXY = uv * 2.0 - 1.0;
    float4 relativeH = mul(vc.InvViewRotProj, float4(ndcXY, depth, 1.0));
    return mul((float3x3)vc.View, relativeH.xyz / relativeH.w);
}

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
    SamplerState samp = g_Samplers[NonUniformResourceIndex(g_PC.Sampler)];
    ViewConstants vc = LoadViewConstants(g_PC.ViewConstantsIndex);
    float2 uv = input.v_UV;
    Texture2D depthTexture = g_Textures[NonUniformResourceIndex(g_PC.DepthTexture)];
    Texture2D hiZ = g_Textures[NonUniformResourceIndex(g_PC.HiZTexture)];

    float depth = depthTexture.Sample(samp, uv).r;
    // The cleared background (reverse-Z far = 0.0) has no surface to reflect from.
    if (depth <= 0.0)
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
    // view-space segment projects to a straight segment in (uv, depth).
    float2 startUV = ViewToScreenUV(vc.Proj, viewPos);
    float4 endClip = mul(vc.Proj, float4(endPos, 1.0));
    float3 endNdc = endClip.xyz / endClip.w;
    float3 p0 = float3(startUV, depth);
    float3 p1 = float3(endNdc.xy * 0.5 + 0.5, endNdc.z);
    // A finite far plane is reverse-Z depth 0: end the line there, where nothing lies beyond.
    if (p1.z < 0.0)
    {
        p1 = lerp(p0, p1, p0.z / (p0.z - p1.z));
    }
    float3 delta = p1 - p0;

    // One step advances the longer screen axis by one texel.
    float2 pixelSpan = abs(delta.xy) * float2(g_PC.Extent);
    float stepCount = max(max(pixelSpan.x, pixelSpan.y), 1.0);
    float3 stepVec = delta / stepCount;
    float2 stepTexels = stepVec.xy * float2(g_PC.Extent);

    // The ray begins at its own surface and steps away in front of the scene, so the first real
    // surface it meets is a front→behind crossing, not an immediate hit: the previous gap
    // (stored - ray, view Z) starts negative.
    float t = 1.0;
    float3 prevRay = p0;
    float prevRayZ = viewPos.z;
    float prevDeltaZ = -1.0;
    bool hit = false;
    float2 hitUV = float2(0.0, 0.0);

    const uint topLevel = max(g_PC.HiZLevels, 1u) - 1u;
    const uint restartLevel = min(HiZStartLevel, topLevel);
    uint level = restartLevel;
    uint testsLeft = max(g_PC.MaxSteps, 1u);
    uint probesLeft = testsLeft;

    while (t <= stepCount && testsLeft > 0u)
    {
        float3 ray = p0 + stepVec * t;

        // The ray left the screen — nothing more to march.
        if (any(ray.xy < 0.0) || any(ray.xy > 1.0))
            break;

        // Hi-Z probe: the cell's max-Z is its nearest surface under reverse-Z. A ray in front of
        // it is in front of everything in the cell, and stays so until it leaves the cell or —
        // heading deeper — reaches that depth, so the whole stretch is skipped. The sign of the
        // ray's depth slope decides which bound applies, at any magnitude.
        if (level > 0u && probesLeft > 0u)
        {
            --probesLeft;
            uint2 levelSize;
            uint levelCount;
            hiZ.GetDimensions(level, levelSize.x, levelSize.y, levelCount);
            float2 texelsPerCell = float2(g_PC.Extent) / float2(levelSize);
            float2 position = ray.xy * float2(g_PC.Extent);
            int2 cell = clamp(int2(floor(position / texelsPerCell)), int2(0, 0), int2(levelSize) - 1);
            float cellMaxZ = hiZ.Load(int3(cell, int(level))).r;
            if (ray.z > cellMaxZ)
            {
                float2 cellLow = float2(cell) * texelsPerCell;
                float2 cellHigh = cellLow + texelsPerCell;
                float exitX = abs(stepTexels.x) > 1e-6
                                  ? ((stepTexels.x > 0.0 ? cellHigh.x : cellLow.x) - position.x) / stepTexels.x
                                  : 1e30;
                float exitY = abs(stepTexels.y) > 1e-6
                                  ? ((stepTexels.y > 0.0 ? cellHigh.y : cellLow.y) - position.y) / stepTexels.y
                                  : 1e30;
                float exitSteps = max(min(exitX, exitY), 0.0);
                float surfaceSteps = stepVec.z < 0.0 ? (cellMaxZ - ray.z) / stepVec.z : 1e30;
                bool leavesCell = exitSteps <= surfaceSteps;
                t += leavesCell ? exitSteps + CellExitBias : surfaceSteps;
                prevRay = p0 + stepVec * t;
                prevRayZ = ViewPositionAt(vc, prevRay.xy, prevRay.z).z;
                prevDeltaZ = -1.0;
                level = leavesCell ? min(level + 1u, topLevel) : level - 1u;
                continue;
            }
            level -= 1u;
            continue;
        }

        // A depth test. The stride stays one texel unless the tests left cannot otherwise reach
        // the end of the line.
        --testsLeft;
        float stride = max(1.0, (stepCount - t) / float(max(testsLeft, 1u)));
        level = restartLevel;

        float rayZ = ViewPositionAt(vc, ray.xy, ray.z).z;
        float sampleDepth = depthTexture.Sample(samp, ray.xy).r;
        if (sampleDepth <= 0.0)
        {
            // No geometry here (background): the ray is in front of nothing, so reset the
            // crossing state rather than carrying a stale "behind" gap across the gap.
            prevRay = ray;
            prevRayZ = rayZ;
            prevDeltaZ = -1.0;
            t += stride;
            continue;
        }

        // Compare in view space so the gap is a real distance, not an NDC-depth delta (which is
        // wildly non-linear). deltaZ > 0 means the ray is behind the stored surface.
        float storedZ = ViewPositionAt(vc, ray.xy, sampleDepth).z;
        float deltaZ = storedZ - rayZ;
        float tolerance = max(g_PC.Thickness, StepToleranceScale * abs(rayZ - prevRayZ));

        if (prevDeltaZ < 0.0 && deltaZ >= 0.0 && deltaZ < tolerance)
        {
            // Bracketed a crossing between prevRay and ray; bisect in screen space to refine.
            float3 lo = prevRay;
            float3 hi = ray;
            [unroll]
            for (uint b = 0; b < 5; ++b)
            {
                float3 mid = (lo + hi) * 0.5;
                float midDepth = depthTexture.Sample(samp, mid.xy).r;
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
        prevRayZ = rayZ;
        prevDeltaZ = deltaZ;
        t += stride;
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
