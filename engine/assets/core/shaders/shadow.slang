// The dedicated directional-shadow system (set 1, off bindless): the depth atlas,
// an immutable comparison sampler for hardware SampleCmp, and the per-frame
// ShadowConstants dynamic uniform. The atlas is one 2D image holding all cascades
// in tiles; each cascade's ShadowConstants matrix bakes in the tile UV remap, so
// mul(CascadeViewProj[k], world) lands in cascade k's tile. The atlas is reverse-Z
// (near→1, far→0), so a lit fragment has the larger depth and the comparison sampler
// does the greater-or-equal compare in hardware with a 2×2 PCF; this header owns the
// cascade select, the per-tile SampleCmp with scaled bias + tile-UV clamp, and the
// cascade-boundary cross-fade.
static const uint MaxCascades = 4;

struct ShadowConstants
{
    float4x4 CascadeViewProj[MaxCascades]; // tile-remap baked in
    float4   CascadeSplits;                // per-cascade view-space far distance
    float4   ShadowParams;                 // x 1/tileRes, y blend-band, z count, w enabled
    float4   CascadeTexelSize;             // per-cascade world units per shadow texel
    float4   CascadeDepthRange;            // per-cascade render ortho depth extent, world units
};

[[vk::binding(0, 3)]] Texture2D g_ShadowAtlas;
[[vk::binding(1, 3)]] SamplerComparisonState g_ShadowSampler;
[[vk::binding(2, 3)]] ConstantBuffer<ShadowConstants> g_Shadow;

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
    float4               AtlasParams; // x 1/tileRes (per-tile texel size), yzw pad
};

[[vk::binding(3, 3)]] ConstantBuffer<PunctualShadows> g_Punctual;
[[vk::binding(4, 3)]] Texture2D g_PunctualAtlas;

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

// Project worldPos by a tile-remapped view-proj and PCF-SampleCmp the atlas tile it
// lands in. The matrix bakes in the tile UV remap (cascade or punctual record), so
// the projected NDC.xy already lies in the tile's atlas window. The reference depth
// carries the supplied bias; every tap UV (including the kernel offsets) is clamped
// to the tile sub-rect [tileMin, tileMax] inset by a half-texel so a tap near a tile
// edge cannot bleed into a neighbouring tile — the atlas's one cost over a texture
// array. Returns full visibility when the projected fragment falls outside the tile
// or the valid depth range. Shared by the cascade and punctual paths over the two
// atlases, selected by the atlas + sampler the caller passes.
float SampleAtlasTile(Texture2D atlas, SamplerComparisonState cmp, float4x4 tileViewProj,
                      float3 worldPos, float bias, float2 texel, float2 tileMin, float2 tileMax)
{
    float4 lightClip = mul(tileViewProj, float4(worldPos, 1.0));
    float3 lightNdc = lightClip.xyz / lightClip.w;

    if (lightNdc.z < 0.0 || lightNdc.z > 1.0)
        return 1.0;

    // The tile remap already lands NDC.xy in the tile's atlas window; convert to UV.
    float2 shadowUV = lightNdc.xy * 0.5 + 0.5;
    if (any(shadowUV < tileMin) || any(shadowUV > tileMax))
        return 1.0;

    float2 clampMin = tileMin + 0.5 * texel;
    float2 clampMax = tileMax - 0.5 * texel;

    // Reverse-Z: the bias pushes the reference toward the light, which is the larger depth.
    float refDepth = lightNdc.z + bias;

    float visibility = 0.0;
    float weight = 0.0;
    for (int dy = -ShadowPcfRadius; dy <= ShadowPcfRadius; ++dy)
        for (int dx = -ShadowPcfRadius; dx <= ShadowPcfRadius; ++dx)
        {
            float2 tapUV = shadowUV + float2(float(dx), float(dy)) * texel;
            tapUV = clamp(tapUV, clampMin, clampMax);
            visibility += atlas.SampleCmpLevelZero(cmp, tapUV, refDepth);
            weight += 1.0;
        }
    return visibility / weight;
}

// Sample cascade k's atlas tile with a PCF grid of hardware comparison taps. Acne is
// held off by two texel-proportional terms, both in world units so they track cascade
// coarseness and stay independent of the light-space depth range: a normal-offset that
// displaces the sample point off the surface, and a depth bias converted to the
// compare's NDC-z through the cascade's actual depth extent.
float SampleCascade(float3 worldPos, float3 N, float3 L, uint cascade, uint count)
{
    float texel = g_Shadow.ShadowParams.x; // 1 / tileRes (atlas-relative texel size)

    // The cascade's tile sub-rect in atlas UV: tile count is min(count,2) columns ×
    // ceil(count/2) rows, matching the renderer's atlas grid.
    uint columns = min(count, 2u);
    uint rows = (count + 1u) / 2u;
    float2 tileSize = float2(1.0 / float(columns), 1.0 / float(rows));
    float2 tileMin = float2(float(cascade % columns), float(cascade / columns)) * tileSize;
    float2 tileMax = tileMin + tileSize;

    float nDotL = saturate(dot(N, L));
    float slope = clamp(1.0 - nDotL, 0.0, 1.0);
    float worldPerTexel = g_Shadow.CascadeTexelSize[cascade];

    // Normal-offset: a PCF tap up to ShadowPcfRadius+1 texels away can land on a
    // depth texel whose world position differs by that many texels across the surface;
    // stepping the sample point one-or-so texels off the surface keeps the compare on
    // the lit side. Grows toward grazing where a texel spans more surface depth.
    float3 samplePos = worldPos + N * (worldPerTexel * (1.0 + 2.0 * slope));

    // Depth bias: a fraction of a texel of world depth, converted to the NDC-z the
    // hardware compare runs in through this cascade's real depth extent.
    float biasWorld = worldPerTexel * (0.5 + 1.0 * slope);
    float bias = biasWorld / max(g_Shadow.CascadeDepthRange[cascade], 1e-4);

    return SampleAtlasTile(g_ShadowAtlas, g_ShadowSampler, g_Shadow.CascadeViewProj[cascade],
                           samplePos, bias, float2(texel, texel), tileMin, tileMax);
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

    // Distance fade: shadows dissolve over the last tenth of the shadowed range, so
    // the range cap (MaxShadowDistance, or the fitted far) reads as a gradient rather
    // than a hard edge where the last cascade ends.
    float lastSplit = g_Shadow.CascadeSplits[count - 1];
    float fadeStart = lastSplit * 0.9;
    visibility = lerp(visibility, 1.0,
                      saturate((viewDepth - fadeStart) / max(lastSplit - fadeStart, 1e-4)));

    return visibility;
}

// Select a point light's cube face from the light→fragment direction (-L, since L
// points surface→light): the largest-magnitude component picks one of the six
// CubeFace-ordered faces (+X,-X,+Y,-Y,+Z,-Z), matching the canonical basis the render
// partitions into. So the sampled face is the one that rendered that direction — the
// cube analogue of the cascade's view-depth select, with no per-face containment test.
uint SelectCubeFace(float3 toFragment)
{
    float3 a = abs(toFragment);
    if (a.x >= a.y && a.x >= a.z)
        return toFragment.x >= 0.0 ? 0u : 1u; // +X, -X
    if (a.y >= a.z)
        return toFragment.y >= 0.0 ? 2u : 3u; // +Y, -Y
    return toFragment.z >= 0.0 ? 4u : 5u;      // +Z, -Z
}

// Per-shadowed-light punctual visibility via hardware SampleCmp, the punctual sibling
// of DirectionalShadowVisibility over the set-1 punctual atlas (binding 4) and the
// shared comparison sampler (binding 1). slot is light.Cone.z (-1 = unshadowed); a
// negative slot returns full visibility with no sample, so an unshadowed light (past
// the budget, or with PunctualShadows off) costs nothing. A spot (Params.x == 2)
// SampleCmps its single tile through Records[slot].ViewProj[0]; a point (Params.x == 1)
// selects the cube face from the major axis of -L and SampleCmps that face's tile. Both
// reuse SampleAtlasTile — the tile remap is baked into the record's matrix, so the
// sample is a projective SampleCmp of the record's tile exactly as a cascade tile is.
float PunctualShadowVisibility(float3 worldPos, float3 N, float3 L, int slot)
{
    if (slot < 0)
        return 1.0;

    PunctualShadowRecord record = g_Punctual.Records[slot];
    float type = record.Params.x;
    if (type < 0.5)
        return 1.0; // an unfilled record (type 0) is unshadowed

    // A point covers six cube-face tiles (face from -L's major axis); a spot covers
    // one (face 0). The face indexes the record's per-face tile-remapped matrix.
    uint face = (type > 1.5) ? 0u : SelectCubeFace(-L);

    // The punctual atlas is CubeFaceCount columns × MaxShadowedPunctual rows of
    // PunctualShadowResolution² tiles. The record at (column face, row slot) is the
    // tile the matrix remap targets; convert to its atlas-UV sub-rect.
    float2 tileSize = float2(1.0 / float(CubeFaceCount), 1.0 / float(MaxShadowedPunctual));
    float2 tileMin = float2(float(face), float(slot)) * tileSize;
    float2 tileMax = tileMin + tileSize;

    // The atlas-relative texel size: one tile spans tileSize of atlas UV over tileRes
    // texels, so the per-axis texel is the per-tile texel (AtlasParams.x = 1/tileRes)
    // scaled by the tile's UV span — anisotropic because the tile grid is non-square.
    float invTileRes = g_Punctual.AtlasParams.x;
    float2 texel = float2(invTileRes / float(CubeFaceCount), invTileRes / float(MaxShadowedPunctual));

    // Per-light bias from the record (the renderer sets it from the light's range and
    // tile resolution — a tile's world-per-texel) plus a slope-scaled term that grows
    // as the surface turns away from the light, mirroring the cascade acne balance.
    float nDotL = saturate(dot(N, L));
    float slope = clamp(1.0 - nDotL, 0.0, 1.0);
    float bias = record.Params.w * (1.0 + 2.0 * slope);

    return SampleAtlasTile(g_PunctualAtlas, g_ShadowSampler, record.ViewProj[face],
                           worldPos, bias, texel, tileMin, tileMax);
}

// A 16-point Poisson disk over the unit circle: the sample pattern for the PCSS blocker search
// and filter. A disk (versus a square grid) spends every tap inside the penumbra footprint, and
// per-fragment rotation (below) turns the residual undersampling into fine dither rather than the
// crawling banding a fixed grid shows as the light or camera moves.
static const float2 AreaPoissonDisk[16] = {
    float2(-0.942016, -0.399062), float2(0.945586, -0.768907), float2(-0.094184, -0.929389),
    float2(0.344959, 0.293878),   float2(-0.915886, 0.457714), float2(-0.815442, -0.879125),
    float2(-0.382775, 0.276768),  float2(0.974844, 0.756484),  float2(0.443233, -0.975116),
    float2(0.537430, -0.473734),  float2(-0.264969, -0.418930), float2(0.791975, 0.190902),
    float2(-0.241888, 0.997065),  float2(-0.814100, 0.914376), float2(0.199841, 0.786414),
    float2(0.143832, -0.141008)};

// Rotate a unit-disk offset by (cos, sin) and scale it to a texel-space step. The tile grid is
// non-square, so the per-axis texel size is anisotropic and applied component-wise.
float2 AreaDiskOffset(float2 disk, float2 rot, float radiusTexels, float2 texel)
{
    float2 rotated = float2(disk.x * rot.x - disk.y * rot.y, disk.x * rot.y + disk.y * rot.x);
    return rotated * radiusTexels * texel;
}

// PCSS area-light shadow visibility over the punctual atlas. An area light is rendered as a
// spot-style caster (record type 2, face 0) in slot `slot`; this samples that tile with
// percentage-closer soft shadows — a blocker search sets an average occluder depth, the penumbra
// width grows with the light's angular size (lightRadius over the fitted far distance) and the
// receiver→blocker gap, and a variable-radius Poisson filter produces the soft edge. Returns full
// visibility outside the tile or with no occluder, so a large light casts a broad, soft shadow and
// a small one a crisp one.
float AreaShadowSample(int slot, float3 worldPos, float3 N, float3 L, float lightRadius)
{
    PunctualShadowRecord record = g_Punctual.Records[slot];
    if (record.Params.x < 0.5)
    {
        return 1.0;
    }

    float4 lightClip = mul(record.ViewProj[0], float4(worldPos, 1.0));
    if (lightClip.w <= 0.0)
    {
        return 1.0;
    }
    float3 ndc = lightClip.xyz / lightClip.w;
    if (ndc.z < 0.0 || ndc.z > 1.0)
    {
        return 1.0;
    }
    float2 uv = ndc.xy * 0.5 + 0.5;

    // The record's tile: an area/spot caster uses face 0, so column 0, row = slot.
    float2 tileSize = float2(1.0 / float(CubeFaceCount), 1.0 / float(MaxShadowedPunctual));
    float2 tileMin = float2(0.0, float(slot)) * tileSize;
    float2 tileMax = tileMin + tileSize;
    if (any(uv < tileMin) || any(uv > tileMax))
    {
        return 1.0;
    }

    float invTileRes = g_Punctual.AtlasParams.x;
    float2 texel =
        float2(invTileRes / float(CubeFaceCount), invTileRes / float(MaxShadowedPunctual));
    float2 clampMin = tileMin + 0.5 * texel;
    float2 clampMax = tileMax - 0.5 * texel;

    float nDotL = saturate(dot(N, L));
    float slope = clamp(1.0 - nDotL, 0.0, 1.0);
    float bias = record.Params.w * (1.0 + 2.0 * slope);
    // Reverse-Z: the bias pushes the reference toward the light, which is the larger depth.
    float refDepth = ndc.z + bias;

    uint atlasW = 0;
    uint atlasH = 0;
    g_PunctualAtlas.GetDimensions(atlasW, atlasH);
    float2 atlasDims = float2(float(atlasW), float(atlasH));

    // Per-fragment disk rotation from a hash of the fragment's shadow texel. Keyed on the shadow-map
    // coordinate (not the screen), so the rotation is stable frame to frame for a given surface
    // point — the noise does not crawl as the camera moves.
    int2 texelCoord = int2(uv * atlasDims);
    float hash = frac(sin(dot(float2(texelCoord), float2(12.9898, 78.233))) * 43758.5453);
    float angle = hash * 6.2831853;
    float2 rot = float2(cos(angle), sin(angle));

    // The light's angular size (world radius over the fitted far distance) sets how wide the search
    // and penumbra grow — the larger the light subtends, the softer the shadow.
    float range = max(record.Params.z, 1e-3);
    float sizeFactor = saturate(lightRadius / range);

    // Blocker search: average the depth of Poisson taps nearer the light than the receiver.
    // Reverse-Z: nearer the light is the larger depth, so a blocker has d > refDepth.
    const int BlockerTaps = 12;
    float searchTexels = 1.5 + sizeFactor * 24.0;
    float blockerSum = 0.0;
    float blockerCount = 0.0;
    [unroll]
    for (int i = 0; i < BlockerTaps; ++i)
    {
        float2 sUv = clamp(uv + AreaDiskOffset(AreaPoissonDisk[i], rot, searchTexels, texel),
                           clampMin, clampMax);
        float d = g_PunctualAtlas.Load(int3(int2(sUv * atlasDims), 0)).r;
        if (d > refDepth)
        {
            blockerSum += d;
            blockerCount += 1.0;
        }
    }
    if (blockerCount < 0.5)
    {
        return 1.0;
    }

    float avgBlocker = blockerSum / blockerCount;

    // PCSS penumbra: proportional to the receiver→blocker gap and the light's size, in texels.
    // Reverse-Z: the blocker sits nearer the light (larger depth), so the gap is avgBlocker − refDepth.
    float penumbra = (avgBlocker - refDepth) / max(avgBlocker, 1e-4);
    float filterTexels = clamp(1.0 + penumbra * sizeFactor * 64.0, 1.0, 24.0);

    // Variable-radius PCF over the full rotated Poisson disk through the hardware comparison sampler.
    const int PcfTaps = 16;
    float visibility = 0.0;
    [unroll]
    for (int i = 0; i < PcfTaps; ++i)
    {
        float2 tapUv = clamp(uv + AreaDiskOffset(AreaPoissonDisk[i], rot, filterTexels, texel),
                             clampMin, clampMax);
        visibility += g_PunctualAtlas.SampleCmpLevelZero(g_ShadowSampler, tapUv, refDepth);
    }
    return visibility / float(PcfTaps);
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
