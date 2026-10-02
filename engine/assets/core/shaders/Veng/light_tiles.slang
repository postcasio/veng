// The per-tile light masks: the screen is cut into LightTileSize-pixel square tiles, and for each
// tile a cull pass (light_tile_cull.comp) writes one 32-bit word naming the view's lights that can
// reach any surface the tile shows — bit i the view's i-th light, as in AllLightsMask. A pass
// shading a pixel then visits only its tile's lights. The words live in a renderer-owned storage
// buffer the view block names (ViewConstants.LightTiles), one region per frame in flight, laid out
// row-major over the tile grid of the view's render allocation.
//
// The masks describe the opaque depth range of each tile, so they serve a pixel at that depth — the
// deferred pass. A surface in front of the opaque depth (a translucent fragment) is not covered by
// them.
//
// The includer declares the view block (view_constants.slang) before including this.

// The tile edge in pixels. Mirrors LightTileSize in the renderer.
static const uint LightTileSize = 16;

// ViewConstants.LightTiles.x when no cull ran for the view: every light applies to every pixel.
static const uint LightTilesNone = 0xFFFFFFFFu;

// Whether the view carries per-tile light masks this frame.
bool HasLightTiles(ViewConstants vc)
{
    return vc.LightTiles.x != LightTilesNone;
}

// The 32-bit word index, within the mask buffer, of the given tile's mask this frame.
uint LightTileWord(ViewConstants vc, uint2 tile)
{
    return vc.LightTiles.z + tile.y * vc.LightTiles.y + tile.x;
}
