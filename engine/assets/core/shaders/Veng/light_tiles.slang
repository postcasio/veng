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
