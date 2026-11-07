// The per-tile light masks: the screen is cut into LightTileSize-pixel square tiles, and for each
// tile a cull pass (light_tile_cull.comp) writes a record of two LightMasks naming the view's lights
// that can reach the tile — bit i of the mask the view's i-th light, as in AllLightsMask. A pass
// shading a pixel then visits only its tile's lights. The records live in a renderer-owned storage
// buffer the view block names (ViewConstants.LightTiles), one region per frame in flight, laid out
// row-major over the tile grid of the view's render allocation.
//
// A record's first mask describes the opaque depth range of its tile, so it serves a pixel at that
// depth — the deferred pass. Its second, the forward mask, describes the tile's frustum from the
// camera out to the tile's farthest opaque surface (unbounded where any of its pixels shows none),
// grown by a pixel on every side, so it serves a surface drawn in front of the opaque depth — a
// forward-lit translucent fragment, found by projecting its world position (ForwardTileLightMask).
//
// The includer declares the view block (view_constants.slang), the light buffer
// (Veng/light_buffer.slang) and the set-0 storage-buffer array g_Buffers before including this.

#pragma once

// The tile edge in pixels. Mirrors LightTileSize in the renderer.
static const uint LightTileSize = 16;

// The 32-bit words in one tile record: the opaque-slice mask, then the forward mask. Mirrors
// LightTileRecordWords in the renderer.
static const uint LightTileRecordWords = 2u * LightMaskWords;

// ViewConstants.LightTiles.x when no cull ran for the view: every light applies to every pixel.
static const uint LightTilesNone = 0xFFFFFFFFu;

// Whether the view carries per-tile light masks this frame.
bool HasLightTiles(ViewConstants vc)
{
    return vc.LightTiles.x != LightTilesNone;
}

// The index, within the mask buffer, of the first 32-bit word of the given tile's record this frame.
uint LightTileRecord(ViewConstants vc, uint2 tile)
{
    return vc.LightTiles.z + (tile.y * vc.LightTiles.y + tile.x) * LightTileRecordWords;
}

// The extent in pixels the cull's tile grid was laid over (ViewConstants.LightTiles.w, x in the low
// half-word and y in the high).
uint2 LightTileExtent(ViewConstants vc)
{
    return uint2(vc.LightTiles.w & 0xFFFFu, vc.LightTiles.w >> 16u);
}

// The mask starting at the given word of the mask buffer.
LightMask LoadLightMask(ViewConstants vc, uint word)
{
    LightMask mask;
    for (uint i = 0u; i < LightMaskWords; ++i)
    {
        mask.Words[i] = g_Buffers[NonUniformResourceIndex(vc.LightTiles.x)].Load((word + i) * 4u);
    }
    return mask;
}

// The lights that can reach an opaque pixel at fragCoord, or every light when the view has no masks.
LightMask TileLightMask(ViewConstants vc, float2 fragCoord)
{
    if (!HasLightTiles(vc))
    {
        return AllLightsMask(vc.LightState.y);
    }
    return LoadLightMask(vc, LightTileRecord(vc, uint2(fragCoord) / LightTileSize));
}

// The lights that can reach a surface drawn at worldPos in front of the opaque depth, or every light
// when the view has no masks or the point does not project into the culled extent.
LightMask ForwardTileLightMask(ViewConstants vc, float3 worldPos)
{
    if (!HasLightTiles(vc))
    {
        return AllLightsMask(vc.LightState.y);
    }
    float3 rel = worldPos - vc.CameraPosition.xyz;
    float4 clip = mul(vc.Proj, float4(mul(vc.View, float4(rel, 0.0)).xyz, 1.0));
    if (clip.w <= 0.0)
    {
        return AllLightsMask(vc.LightState.y);
    }
    float2 uv = clip.xy / clip.w * 0.5 + 0.5;
    uint2 extent = LightTileExtent(vc);
    float2 pixel = uv * float2(extent);
    if (any(pixel < 0.0) || any(pixel >= float2(extent)))
    {
        return AllLightsMask(vc.LightState.y);
    }
    return LoadLightMask(vc, LightTileRecord(vc, uint2(pixel) / LightTileSize) + LightMaskWords);
}

// The most tiles ForwardSphereLightMask unions before it takes every light instead: past this,
// reading the masks costs more than the lights they would skip.
static const uint ForwardSphereMaxTiles = 64u;

// The lights that can reach any part of the world-space sphere (centre, radius) drawn in front of
// the opaque depth: the union of the forward masks of every tile the sphere's projection covers. A
// light reaching any visible point of the sphere is in the mask of the tile that point projects to,
// so it is kept however the sphere straddles tile edges; what the union leaves out reaches no
// visible part of it. The projection is bounded by the view-space box around the sphere, through
// the same projection the forward fragments use.
// - With no masks this frame, every light.
// - A sphere reaching the camera plane, or covering more than ForwardSphereMaxTiles tiles, takes
//   every light.
// - A sphere whose projection lies wholly outside the culled extent is not seen; it takes the
//   directional lights alone (DirectionalLightsMask).
LightMask ForwardSphereLightMask(ViewConstants vc, float3 centre, float radius)
{
    uint count = vc.LightState.y;
    if (!HasLightTiles(vc))
    {
        return AllLightsMask(count);
    }
    float3 c = mul(vc.View, float4(centre - vc.CameraPosition.xyz, 0.0)).xyz;
    float2 lo = float2(3.4e38);
    float2 hi = float2(-3.4e38);
    for (uint k = 0u; k < 8u; ++k)
    {
        float3 corner = c + radius * float3((k & 1u) != 0u ? 1.0 : -1.0,
                                            (k & 2u) != 0u ? 1.0 : -1.0,
                                            (k & 4u) != 0u ? 1.0 : -1.0);
        float4 clip = mul(vc.Proj, float4(corner, 1.0));
        if (!(clip.w > 0.0))
        {
            return AllLightsMask(count);
        }
        float2 ndc = clip.xy / clip.w;
        lo = min(lo, ndc);
        hi = max(hi, ndc);
    }
    uint2 extent = LightTileExtent(vc);
    float2 pixelLo = (lo * 0.5 + 0.5) * float2(extent);
    float2 pixelHi = (hi * 0.5 + 0.5) * float2(extent);
    if (any(pixelHi < 0.0) || any(pixelLo >= float2(extent)))
    {
        return DirectionalLightsMask(vc);
    }
    uint2 first = uint2(max(pixelLo, float2(0.0))) / LightTileSize;
    uint2 last = uint2(min(pixelHi, float2(extent) - 1.0)) / LightTileSize;
    uint2 span = last - first + 1u;
    if (span.x * span.y > ForwardSphereMaxTiles)
    {
        return AllLightsMask(count);
    }

    LightMask mask;
    for (uint i = 0u; i < LightMaskWords; ++i)
    {
        mask.Words[i] = 0u;
    }
    uint words = min((count + 31u) / 32u, LightMaskWords);
    for (uint y = first.y; y <= last.y; ++y)
    {
        for (uint x = first.x; x <= last.x; ++x)
        {
            uint word = LightTileRecord(vc, uint2(x, y)) + LightMaskWords;
            for (uint i = 0u; i < words; ++i)
            {
                mask.Words[i] |=
                    g_Buffers[NonUniformResourceIndex(vc.LightTiles.x)].Load((word + i) * 4u);
            }
        }
    }
    return mask;
}
