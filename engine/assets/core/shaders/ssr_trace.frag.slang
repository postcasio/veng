// Screen-space reflection trace: for each reflective fragment, reflects the view
// vector about the g-buffer normal and marches the ray through the depth buffer to
// find the first surface it crosses, then samples the lit scene color there. Writes
// two targets, both premultiplied by the hit's confidence (the screen-edge fade) so the
// blur chain filters them linearly: the reflected radiance with the confidence in alpha,
// and the view-space length of the reflected ray to its hit, which the composite reads
// to size the reflected cone's footprint. A third target holds the surface the ray left from —
// its device depth, that squared, and a coverage of one — for every texel with a surface, whatever
// its ray did, so the composite can tell which surfaces a coarse level's texel averaged.
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
// resolution; the g-buffer it reads is full-resolution (SSR disables the dynamic-resolution
// sub-rect).
//
// Each texel's ray leaves the surface on its unjittered ray (ssr_surface.slang) and is marched in
// the rasterized frame, through the jittered projection over the jittered depth, where every depth
// it compares against lies. The scene colour it reads is the temporal resolve's output when one
// runs, reconstructed at unjittered positions, so the hit is read back JitterNdc / 2 from where the
// march found it. With no temporal resolve the jitter is zero and the two frames coincide.
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
//
// The reflected colour is read through an explicit footprint rather than by implicit derivatives,
// which are undefined past the march's early exits and across a quad whose rays hit different
// surfaces. The footprint is where the neighbouring trace texels' rays land: each neighbour's pixel
// ray meets this ray's surface plane, reflects about the same normal and meets the hit surface's
// tangent plane, and the two landing points' offsets from the hit, in scene-colour texels, span it.
// Rays a footprint apart sample the reflected image on that lattice, and the read is a tent over it:
// four bilinear taps along the footprint's axes add whatever variance the bilinear tap's own (one
// sixth of a texel squared) lacks of the tent's, so a mirror whose rays land a texel apart or closer
// reads one bilinear tap and stays as sharp as the trace, while rays that diverge are prefiltered
// across the gap between them. The footprint is the hit's own tangent plane, so it never spans a
// silhouette the way a quad's derivatives do.
#include "ssr_surface.slang"

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

// The largest hit distance the R16F target stores; a footprint that far away is as wide as
// the cone ever gets on screen.
static const float MaxStoredHitDistance = 60000.0;

// The longest footprint axis the reflected-colour read is prefiltered over, in scene-colour texels:
// a ray grazing the surface it hits spreads its neighbours far along it, and a read that wide would
// smear the hit surface rather than antialias it.
static const float MaxFootprintTexels = 8.0;

// The least cosine between a neighbouring ray and the plane it meets for the footprint to be taken;
// a grazing intersection has no stable landing point, and the read falls back to one bilinear tap.
static const float MinFootprintCosine = 1e-3;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

struct TraceOutput
{
    float4 Reflection : SV_Target0;  // reflected radiance x confidence, confidence
    float HitDistance : SV_Target1;  // view-space ray length to the hit x confidence
    float4 Surface : SV_Target2;     // the ray's surface device depth and its square x coverage, coverage
};

static const TraceOutput Miss = { float4(0.0, 0.0, 0.0, 0.0), 0.0, float4(0.0, 0.0, 0.0, 0.0) };

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

// The landing offset, in scene-colour texels, of the ray leaving the neighbouring trace texel at
// `neighbourUV`: its pixel ray meets the plane through `surface` with normal `normal`, reflects
// about that normal and meets the plane through `hit` with normal `hitNormal`. Zero when either
// meeting is grazing or the landing point is behind the camera.
float2 NeighbourLanding(ViewConstants vc, float2 neighbourUV, float depth, float3 surface, float3 normal,
                        float3 hit, float3 hitNormal, float2 hitUV, float2 sceneSize)
{
    // The pixel ray through the neighbour, from its point at this surface's depth toward one further
    // away, so a perspective and an orthographic projection give it alike.
    float3 near = ViewPositionAt(vc, neighbourUV, depth);
    float3 direction = normalize(ViewPositionAt(vc, neighbourUV, depth * 0.5) - near);
    float towardSurface = dot(normal, direction);
    if (abs(towardSurface) < MinFootprintCosine)
        return float2(0.0, 0.0);
    float3 start = near + direction * (dot(normal, surface - near) / towardSurface);
    float3 reflected = reflect(direction, normal);
    float towardHit = dot(hitNormal, reflected);
    if (abs(towardHit) < MinFootprintCosine)
        return float2(0.0, 0.0);
    float3 landing = start + reflected * (dot(hitNormal, hit - start) / towardHit);
    if (landing.z >= 0.0)
        return float2(0.0, 0.0);
    float2 offset = (ViewToScreenUV(vc.Proj, landing) - hitUV) * sceneSize;
    float length2 = dot(offset, offset);
    return length2 > MaxFootprintTexels * MaxFootprintTexels ? offset * (MaxFootprintTexels * rsqrt(length2))
                                                             : offset;
}

// The tap offset along one footprint axis: the share of the axis whose square, as a variance, is
// what the tent over the axis needs beyond the bilinear tap's own sixth of a texel squared.
float2 TapSpread(float2 axis)
{
    float length2 = dot(axis, axis);
    if (length2 <= 1.0)
        return float2(0.0, 0.0);
    return axis * sqrt((1.0 - 1.0 / length2) / 6.0);
}

[shader("fragment")]
TraceOutput fsMain(VSOutput input)
{
    SamplerState samp = g_Samplers[NonUniformResourceIndex(g_PC.Sampler)];
    ViewConstants vc = LoadViewConstants(g_PC.ViewConstantsIndex);
    float2 uv = input.v_UV;
    Texture2D depthTexture = g_Textures[NonUniformResourceIndex(g_PC.DepthTexture)];
    Texture2D normalTexture = g_Textures[NonUniformResourceIndex(g_PC.NormalTexture)];
    Texture2D hiZ = g_Textures[NonUniformResourceIndex(g_PC.HiZTexture)];

    // The cleared background has no surface to reflect from.
    SsrSurface surface = SsrSurfaceAt(vc, depthTexture, normalTexture, uv);
    if (!surface.Valid)
        return Miss;

    // The ray's start in the rasterized frame's screen space (uv.xy + NDC depth).
    float3 viewPos = mul((float3x3)vc.View, surface.Position);
    float4 startClip = mul(vc.Proj, float4(viewPos, 1.0));
    float3 startNdc = startClip.xyz / startClip.w;
    float2 startUV = startNdc.xy * 0.5 + 0.5;
    float depth = startNdc.z;
    TraceOutput missed = Miss;
    missed.Surface = float4(depth, depth * depth, 1.0, 0.0);

    float roughness =
        g_Textures[NonUniformResourceIndex(g_PC.OrmTexture)].Load(int3(surface.Texel, 0)).g;
    // Very rough surfaces reflect nothing coherent; skip the trace (the composite fades the
    // reflection out smoothly below this cutoff).
    if (roughness > g_PC.MaxRoughness)
        return missed;

    float3 N = mul((float3x3)vc.View, surface.Normal);

    // The camera is at the view-space origin, so the surface's view position is also the view ray
    // to it; reflect that about the surface normal.
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
        return missed;
    float3 endPos = viewPos + R * endDistance;

    // Project the ray's end the same way. A straight view-space segment projects to a straight
    // segment in (uv, depth).
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
    float3 hitPos = float3(0.0, 0.0, 0.0);
    float hitDistance = 0.0;

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
        float sampleDepth = depthTexture.SampleLevel(samp, ray.xy, 0.0).r;
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
                float midDepth = depthTexture.SampleLevel(samp, mid.xy, 0.0).r;
                float midStored = ViewPositionAt(vc, mid.xy, midDepth).z;
                float midRay = ViewPositionAt(vc, mid.xy, mid.z).z;
                if (midStored - midRay > 0.0)
                    hi = mid;
                else
                    lo = mid;
            }
            hitUV = hi.xy;
            hitPos = ViewPositionAt(vc, hi.xy, hi.z);
            hitDistance = length(hitPos - viewPos);
            hit = true;
            break;
        }

        prevRay = ray;
        prevRayZ = rayZ;
        prevDeltaZ = deltaZ;
        t += stride;
    }

    if (!hit)
        return missed;

    // The reflected colour, prefiltered over the footprint the neighbouring rays' hits span and
    // read where the scene colour holds the hit: unjittered, as the temporal resolve wrote it.
    Texture2D sceneColor = g_Textures[NonUniformResourceIndex(g_PC.SceneColorTexture)];
    uint sceneWidth;
    uint sceneHeight;
    uint sceneLevels;
    sceneColor.GetDimensions(0u, sceneWidth, sceneHeight, sceneLevels);
    float2 sceneSize = float2(sceneWidth, sceneHeight);
    uint normalWidth;
    uint normalHeight;
    uint normalLevels;
    normalTexture.GetDimensions(0u, normalWidth, normalHeight, normalLevels);
    int2 hitTexel = clamp(int2(hitUV * float2(normalWidth, normalHeight)), int2(0, 0),
                          int2(normalWidth, normalHeight) - 1);
    float3 hitWorldNormal = normalTexture.Load(int3(hitTexel, 0)).xyz;
    float hitNormalLength = length(hitWorldNormal);
    float2 spreadX = float2(0.0, 0.0);
    float2 spreadY = float2(0.0, 0.0);
    if (hitNormalLength > 1e-4)
    {
        float3 hitNormal = mul((float3x3)vc.View, hitWorldNormal / hitNormalLength);
        float2 texel = 1.0 / float2(g_PC.Extent);
        spreadX = TapSpread(NeighbourLanding(vc, startUV + float2(texel.x, 0.0), depth, viewPos, N,
                                             hitPos, hitNormal, hitUV, sceneSize));
        spreadY = TapSpread(NeighbourLanding(vc, startUV + float2(0.0, texel.y), depth, viewPos, N,
                                             hitPos, hitNormal, hitUV, sceneSize));
    }
    float2 resolvedUV = hitUV - vc.JitterNdc.xy * 0.5;
    float3 reflected;
    if (all(spreadX == 0.0) && all(spreadY == 0.0))
    {
        reflected = sceneColor.SampleLevel(samp, resolvedUV, 0.0).rgb;
    }
    else
    {
        float2 offsetA = (spreadX + spreadY) / sceneSize;
        float2 offsetB = (spreadX - spreadY) / sceneSize;
        reflected = 0.25 * (sceneColor.SampleLevel(samp, resolvedUV + offsetA, 0.0).rgb +
                            sceneColor.SampleLevel(samp, resolvedUV - offsetA, 0.0).rgb +
                            sceneColor.SampleLevel(samp, resolvedUV + offsetB, 0.0).rgb +
                            sceneColor.SampleLevel(samp, resolvedUV - offsetB, 0.0).rgb);
    }

    // Fade the contribution as the hit nears the screen border (the reflected detail
    // runs off-screen there) so reflections vanish smoothly rather than clipping.
    float2 edge = smoothstep(0.0, 0.15, resolvedUV) * (1.0 - smoothstep(0.85, 1.0, resolvedUV));
    float confidence = edge.x * edge.y;

    TraceOutput output = missed;
    output.Reflection = float4(reflected * confidence, confidence);
    output.HitDistance = min(hitDistance, MaxStoredHitDistance) * confidence;
    return output;
}
