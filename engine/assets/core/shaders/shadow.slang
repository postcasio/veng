// The dedicated directional-shadow system (set 1, off bindless): the depth atlas,
// an immutable comparison sampler for hardware SampleCmp, and the per-frame
// ShadowConstants dynamic uniform. The atlas is one 2D image holding all cascades
// in tiles; each cascade's ShadowConstants matrix bakes in the tile UV remap, so
// mul(CascadeViewProj[k], world) lands in cascade k's tile. The comparison sampler
// does the LESS-or-equal compare in hardware with a 2×2 PCF; this header owns the
// cascade select, the per-tile SampleCmp with scaled bias + tile-UV clamp, and the
// cascade-boundary cross-fade.
static const uint MaxCascades = 4;

struct ShadowConstants
{
    float4x4 CascadeViewProj[MaxCascades]; // tile-remap baked in
    float4   CascadeSplits;                // per-cascade view-space far distance
    float4   ShadowParams;                 // x 1/tileRes, y blend-band, z count, w enabled
};

[[vk::binding(0, 1)]] Texture2D g_ShadowAtlas;
[[vk::binding(1, 1)]] SamplerComparisonState g_ShadowSampler;
[[vk::binding(2, 1)]] ConstantBuffer<ShadowConstants> g_Shadow;

// The shadow system also carries a bounded set of punctual (point/spot) shadows.
// MaxShadowedPunctual lights get a record; the lighting pass reads light.Cone.z as
// the slot (-1 = unshadowed) and SampleCmps Records[slot] through the SAME comparison
// sampler (binding 1) and a SECOND 2D depth atlas (binding 4), packed in tiles exactly
// as the cascade atlas is. CubeFaceCount is the cube-face count: 1 view for a spot
// (ViewProj[0]), 6 for a point (ViewProj[0..5]) — each with the atlas tile-remap baked
// in, so a fragment projected by a record's matrix lands in that light's tile.
static const uint MaxShadowedPunctual = 4;
static const uint CubeFaceCount = 6;

struct PunctualShadowRecord
{
    float4x4 ViewProj[CubeFaceCount]; // tile-remap baked in ([0] spot, [0..5] point)
    float4   PositionRange;           // xyz world position, w range
    float4   Params;                  // x type (1 point / 2 spot / 0 none), y near, z far, w bias
};

struct PunctualShadows
{
    PunctualShadowRecord Records[MaxShadowedPunctual];
};

[[vk::binding(3, 1)]] ConstantBuffer<PunctualShadows> g_Punctual;
[[vk::binding(4, 1)]] Texture2D g_PunctualAtlas;

// The PCF tap grid half-extent: a (2*Radius+1)² kernel of comparison taps, each
// offset by the atlas texel size and individually hardware-filtered, widening the
// penumbra past the comparison sampler's own 2×2 bilinear footprint.
static const int ShadowPcfRadius = 1;

// Select the cascade by the fragment's view-space depth (positive distance in front
// of the camera) against the per-cascade far splits. Returns count when the fragment
// is past the last cascade's far plane (no cascade covers it).
uint SelectCascade(float viewDepth, uint count)
{
    for (uint i = 0; i < count; ++i)
    {
        if (viewDepth <= g_Shadow.CascadeSplits[i])
            return i;
    }
    return count;
}

// The world-units-per-texel of a cascade, recovered from the cascade's clip-space
// extent: a cascade's CascadeViewProj maps its world slice to one atlas tile, so the
// world width covered by the tile is 2 / |column-scale of the projection| folded
// over the tile fraction. Approximated from the matrix's X basis length, which scales
// inversely with the cascade's world extent — coarser far cascades return a larger
// value and so take more bias.
float CascadeWorldPerTexel(uint cascade, uint count)
{
    // The matrix's first column length (the NDC-x per world-x rate) shrinks as the
    // cascade covers more world; its reciprocal grows with the cascade's world span.
    float3 xBasis = float3(g_Shadow.CascadeViewProj[cascade][0].x,
                           g_Shadow.CascadeViewProj[cascade][1].x,
                           g_Shadow.CascadeViewProj[cascade][2].x);
    float ndcPerWorld = max(length(xBasis), 1e-6);
    return 1.0 / ndcPerWorld;
}

// Sample cascade k's atlas tile with a PCF grid of hardware comparison taps. The
// reference depth carries a per-cascade-scaled, slope-scaled bias; every tap UV
// (including the kernel offsets) is clamped to the cascade's tile sub-rect (inset by
// a half-texel) so a tap near a tile edge cannot bleed into a neighbouring cascade's
// tile — the atlas's one cost over a texture array. Returns full visibility when the
// projected fragment falls outside the tile or the valid depth range.
float SampleCascade(float3 worldPos, float3 N, float3 L, uint cascade, uint count)
{
    float4 lightClip = mul(g_Shadow.CascadeViewProj[cascade], float4(worldPos, 1.0));
    float3 lightNdc = lightClip.xyz / lightClip.w;

    // The tile remap already lands NDC.xy in the tile's atlas window; convert to UV.
    float2 shadowUV = lightNdc.xy * 0.5 + 0.5;
    if (lightNdc.z < 0.0 || lightNdc.z > 1.0)
        return 1.0;

    float texel = g_Shadow.ShadowParams.x; // 1 / tileRes (atlas-relative texel size)

    // The cascade's tile sub-rect in atlas UV: tile count is min(count,2) columns ×
    // ceil(count/2) rows, matching the renderer's atlas grid. Clamp every tap inside
    // the tile inset by a half-texel.
    uint columns = min(count, 2u);
    uint rows = (count + 1u) / 2u;
    float2 tileSize = float2(1.0 / float(columns), 1.0 / float(rows));
    float2 tileMin = float2(float(cascade % columns), float(cascade / columns)) * tileSize;
    float2 tileMax = tileMin + tileSize;
    float2 clampMin = tileMin + 0.5 * texel;
    float2 clampMax = tileMax - 0.5 * texel;

    if (any(shadowUV < tileMin) || any(shadowUV > tileMax))
        return 1.0;

    // Per-cascade bias: coarser far cascades cover more world per texel and need more
    // depth bias to stay acne-free. A slope-scaled term grows as the surface turns
    // away from the light. The scale folds the cascade's world-per-texel in.
    float nDotL = saturate(dot(N, L));
    float worldPerTexel = CascadeWorldPerTexel(cascade, count);
    float slope = clamp(1.0 - nDotL, 0.0, 1.0);
    float bias = (0.0008 + 0.0040 * slope) * (worldPerTexel * 64.0);
    bias = clamp(bias, 0.0002, 0.01);
    float refDepth = lightNdc.z - bias;

    float visibility = 0.0;
    float weight = 0.0;
    for (int dy = -ShadowPcfRadius; dy <= ShadowPcfRadius; ++dy)
        for (int dx = -ShadowPcfRadius; dx <= ShadowPcfRadius; ++dx)
        {
            float2 tapUV = shadowUV + float2(float(dx), float(dy)) * texel;
            tapUV = clamp(tapUV, clampMin, clampMax);
            visibility += g_ShadowAtlas.SampleCmpLevelZero(g_ShadowSampler, tapUV, refDepth);
            weight += 1.0;
        }
    return visibility / weight;
}

// Cascaded directional-shadow visibility via hardware SampleCmp. Selects the cascade
// by view-space depth against the per-cascade splits, samples that cascade's atlas
// tile, and cross-fades into the next cascade within a small blend band before the
// selected cascade's far split so the resolution change across a cascade edge reads
// as a gradient, not a hard line. The last cascade fades to full visibility past its
// far plane. The band width rides ShadowParams.y.
float DirectionalShadowVisibility(float3 worldPos, float3 N, float3 L, float viewDepth)
{
    // ShadowParams.w gates the whole path: 0 when the shadow pass is compiled out,
    // so the term is fully lit and the dummy/zeroed atlas is never meaningfully read.
    if (g_Shadow.ShadowParams.w < 0.5)
        return 1.0;

    uint count = uint(g_Shadow.ShadowParams.z);
    if (count == 0)
        return 1.0;

    uint cascade = SelectCascade(viewDepth, count);
    if (cascade >= count)
        return 1.0; // past the last cascade's far plane — unshadowed

    float visibility = SampleCascade(worldPos, N, L, cascade, count);

    // Cross-fade: within a blend band before this cascade's far split, blend toward
    // the next cascade's sample (or toward full visibility past the last cascade).
    float band = g_Shadow.ShadowParams.y;
    if (band > 1e-5)
    {
        float farSplit = g_Shadow.CascadeSplits[cascade];
        float bandStart = farSplit - band;
        if (viewDepth > bandStart)
        {
            float t = saturate((viewDepth - bandStart) / band);
            float nextVisibility = (cascade + 1u < count)
                ? SampleCascade(worldPos, N, L, cascade + 1u, count)
                : 1.0;
            visibility = lerp(visibility, nextVisibility, t);
        }
    }

    return visibility;
}

// Visualize the cascade selection: a per-cascade tint the DebugView::Cascades arm
// multiplies into the lit result so each fragment's selected cascade is a colour
// bucket (0 red, 1 green, 2 blue, 3 yellow). Uses the same view-depth → cascade
// logic as the shadow sample, so the visualization pins the selection it tints.
float3 CascadeDebugTint(float viewDepth)
{
    uint count = uint(g_Shadow.ShadowParams.z);
    if (count == 0)
        return float3(1.0, 1.0, 1.0);

    uint cascade = SelectCascade(viewDepth, count);
    switch (cascade)
    {
        case 0:  return float3(1.0, 0.25, 0.25); // red — near
        case 1:  return float3(0.25, 1.0, 0.25); // green
        case 2:  return float3(0.25, 0.4, 1.0);  // blue
        case 3:  return float3(1.0, 1.0, 0.25);  // yellow — far
        default: return float3(0.4, 0.4, 0.4);   // past the last cascade
    }
}
