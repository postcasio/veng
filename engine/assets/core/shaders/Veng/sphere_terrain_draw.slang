// Sphere terrain's drawing core, shared by the g-buffer, translucent and shadow stages: the terrain
// set's bindings, the per-tile record, the precise placement and the geometric morph. One function
// places a vertex for every stage, so a tile's shadow cannot disagree with its colour.
//
// The terrain set holds the source's pool: channel n's array at binding n (channel 0 the height), the
// frame's tile records at binding 8 and the per-view lists at binding 9. A draw's firstInstance is its
// list start, so SV_VulkanInstanceID names the list entry, and the entry names the record.
//
// VE_SPHERE_TERRAIN_SET names the set: 4 for the g-buffer and shadow stages (the default), 6 for a
// translucent level layer, whose sets 4 and 5 are forward lighting's (Veng/sphere_terrain_translucent.slang).
//
// The engine's C++ twin of the placement and the morph is src/Renderer/SphereTerrainVertex.h; the
// two are kept in step so the unit band can bound the device's arithmetic on the host.
#pragma once

#include "sphere_terrain_mapping.slang"

// One drawn tile; mirrors Veng::Renderer::SphereTerrainDrawRecord (688 bytes, std430).
struct SphereTerrainRecord
{
    // The tile's body-fixed placement (centre direction, corner offsets and norms, warp terms).
    SphereTerrainTileConstants Tile;
    // Per corner (index sx + 2 sy): its position relative to the eye, in metres, scene axes.
    float4 Corners[4];
    // Per corner: its unit vertical, scene axes.
    float4 Verticals[4];
    // |d0-d1|^2, |d0-d2|^2, |d0-d3|^2, |d1-d2|^2.
    float4 ChordsA;
    // |d1-d3|^2, |d2-d3|^2, the radius in metres, and the scale about the eye.
    float4 ChordsB;
    // Per corner: last frame's position in last frame's scene space, relative to this frame's camera.
    float4 PrevCorners[4];
    // The tile's unit tangent along u, scene axes.
    float4 TangentU;
    // Morph start, morph end, arrival floor, skirt depth.
    float4 Morph;
    // The level layers' heights above the radius (xy) and the texel size in metres (z).
    float4 Heights;
    // The tile's layer, its parent's layer, its quadrant in the parent, and the SphereTerrainRecord*
    // flags.
    uint4 Layers;
    // The arc terms of the tile's fraction along u (xy) and v (zw): S(dtheta), C(dtheta) - x0 S(dtheta).
    float4 Arc;
    // The generator's words for the tile, and for its parent.
    uint Data[32];
    uint ParentData[32];
};

// The record flags in Layers.w; mirror the SphereTerrainRecord* constants of
// src/Renderer/SphereTerrainVertex.h.
static const uint SphereTerrainRecordHasParent = 1u;
static const uint SphereTerrainRecordHeld = 2u;
static const uint SphereTerrainRecordTileOverlay = 4u;

#ifndef VE_SPHERE_TERRAIN_SET
#define VE_SPHERE_TERRAIN_SET 4
#endif

[[vk::binding(0, VE_SPHERE_TERRAIN_SET)]] Texture2DArray<float4> g_TerrainChannel0;
[[vk::binding(1, VE_SPHERE_TERRAIN_SET)]] Texture2DArray<float4> g_TerrainChannel1;
[[vk::binding(2, VE_SPHERE_TERRAIN_SET)]] Texture2DArray<float4> g_TerrainChannel2;
[[vk::binding(3, VE_SPHERE_TERRAIN_SET)]] Texture2DArray<float4> g_TerrainChannel3;
[[vk::binding(4, VE_SPHERE_TERRAIN_SET)]] Texture2DArray<float4> g_TerrainChannel4;
[[vk::binding(5, VE_SPHERE_TERRAIN_SET)]] Texture2DArray<float4> g_TerrainChannel5;
[[vk::binding(6, VE_SPHERE_TERRAIN_SET)]] Texture2DArray<float4> g_TerrainChannel6;
[[vk::binding(7, VE_SPHERE_TERRAIN_SET)]] Texture2DArray<float4> g_TerrainChannel7;
[[vk::binding(8, VE_SPHERE_TERRAIN_SET)]] StructuredBuffer<SphereTerrainRecord> g_TerrainRecords;
[[vk::binding(9, VE_SPHERE_TERRAIN_SET)]] StructuredBuffer<uint> g_TerrainList;

// The grid a tile is drawn with: 32 quads a side, vertex k on texel 2 + 4k.
static const uint SphereTerrainGridQuads = 32u;
static const uint SphereTerrainGridRow = SphereTerrainGridQuads + 1u;
static const uint SphereTerrainSurfaceVertices = SphereTerrainGridRow * SphereTerrainGridRow;
static const uint SphereTerrainGridTexelStep = 4u;
static const uint SphereTerrainGridBorder = 2u;
static const uint SphereTerrainTexels = 133u;

// The corner weights w_i = b_i |Q_i| / sum b_j |Q_j| of the warped fractions at tile parameters st.
float4 SphereTerrainCornerWeights(SphereTerrainTileConstants tile, float2 st)
{
    precise float au = SphereTerrainTileFraction(st.x, tile.Warp.x, tile.Warp.y);
    precise float av = SphereTerrainTileFraction(st.y, tile.Warp.z, tile.Warp.w);
    precise float4 beta = float4((1.0 - au) * (1.0 - av), au * (1.0 - av), (1.0 - au) * av, au * av) *
                          float4(tile.Corners[0].w, tile.Corners[1].w, tile.Corners[2].w, tile.Corners[3].w);
    precise float total = beta.x + beta.y + beta.z + beta.w;
    precise float4 weights = beta / total;
    return weights;
}

// The corner weights at tile parameters st from the record's arc terms, so no sine or cosine of the
// tile's span is evaluated per vertex. The tile's edges take fractions of exactly 0 and 1, so
// neighbouring tiles' edge vertices coincide.
float4 SphereTerrainCornerWeights(SphereTerrainRecord r, float2 st)
{
    precise float au = SphereTerrainTileFractionArc(st.x, r.Tile.Warp.x, r.Tile.Warp.y, r.Arc.xy);
    precise float av = SphereTerrainTileFractionArc(st.y, r.Tile.Warp.z, r.Tile.Warp.w, r.Arc.zw);
    precise float4 beta = float4((1.0 - au) * (1.0 - av), au * (1.0 - av), (1.0 - au) * av, au * av) *
                          float4(r.Tile.Corners[0].w, r.Tile.Corners[1].w, r.Tile.Corners[2].w,
                                 r.Tile.Corners[3].w);
    precise float total = beta.x + beta.y + beta.z + beta.w;
    precise float4 weights = beta / total;
    return weights;
}

// The sagitta-and-height lift above the chord point: R (1 - |m|^2) / (1 + |m|) + h, with
// 1 - |m|^2 = sum_{i<j} w_i w_j |d_i - d_j|^2 taken from the record's exact chords.
float SphereTerrainLift(SphereTerrainRecord r, float4 w, float height)
{
    precise float shortfall = w.x * w.y * r.ChordsA.x + w.x * w.z * r.ChordsA.y + w.x * w.w * r.ChordsA.z +
                              w.y * w.z * r.ChordsA.w + w.y * w.w * r.ChordsB.x + w.z * w.w * r.ChordsB.y;
    precise float lift = r.ChordsB.z * (shortfall / (1.0 + sqrt(1.0 - shortfall))) + height;
    return lift;
}

// A placed vertex: its position relative to the eye, unscaled, and the sphere's vertical there.
struct SphereTerrainPlacement
{
    float3 Position;
    float3 Vertical;
};

// Places a vertex of the tile at tile parameters st and a height above the radius:
// sum w_i C_i + n (R (1 - |m|^2) / (1 + |m|) + h). Every term is an eye-relative corner or a
// quantity no larger than the tile, so nothing the size of the radius is subtracted in f32.
SphereTerrainPlacement SphereTerrainPlace(SphereTerrainRecord r, float4 w, float height)
{
    precise float lift = SphereTerrainLift(r, w, height);
    precise float3 vertical = normalize(w.x * r.Verticals[0].xyz + w.y * r.Verticals[1].xyz +
                                        w.z * r.Verticals[2].xyz + w.w * r.Verticals[3].xyz);
    precise float3 chord = w.x * r.Corners[0].xyz + w.y * r.Corners[1].xyz + w.z * r.Corners[2].xyz +
                           w.w * r.Corners[3].xyz;
    SphereTerrainPlacement placement;
    placement.Position = chord + vertical * lift;
    placement.Vertical = vertical;
    return placement;
}

// Places a vertex at tile parameters st, computing their corner weights.
SphereTerrainPlacement SphereTerrainPlace(SphereTerrainRecord r, float2 st, float height)
{
    return SphereTerrainPlace(r, SphereTerrainCornerWeights(r, st), height);
}

// The same vertex as the tile stood last frame, relative to this frame's camera in last frame's
// scene space; the vertical is this frame's, which differs by the sphere's turn in a frame.
float3 SphereTerrainPlacePrevious(SphereTerrainRecord r, float4 w, float height, float3 vertical)
{
    precise float lift = SphereTerrainLift(r, w, height);
    precise float3 chord = w.x * r.PrevCorners[0].xyz + w.y * r.PrevCorners[1].xyz +
                           w.z * r.PrevCorners[2].xyz + w.w * r.PrevCorners[3].xyz;
    return chord + vertical * lift;
}

// The previous place at tile parameters st, computing their corner weights.
float3 SphereTerrainPlacePrevious(SphereTerrainRecord r, float2 st, float height, float3 vertical)
{
    return SphereTerrainPlacePrevious(r, SphereTerrainCornerWeights(r, st), height, vertical);
}

// The morph factor max(saturate((d - S) / (E - S)), floor).
float SphereTerrainMorphFactor(float distance, float4 morph)
{
    const float span = max(morph.y - morph.x, 1e-6);
    return max(saturate((distance - morph.x) / span), morph.z);
}

// A height at a grid vertex of a layer.
float SphereTerrainGridVertexHeight(uint layer, int2 grid)
{
    const int2 texel = int2(SphereTerrainGridBorder) + grid * int(SphereTerrainGridTexelStep);
    return g_TerrainChannel0.Load(int4(texel, int(layer), 0)).r;
}

// A height over a layer's grid, linear over the grid's triangles (each quad cut from its lowest
// corner to its highest) at a fractional grid coordinate.
float SphereTerrainGridHeight(uint layer, float2 grid)
{
    const float2 cell = clamp(floor(grid), float2(0.0), float2(float(SphereTerrainGridQuads) - 1.0));
    const float2 f = grid - cell;
    const int2 c = int2(cell);
    const float h00 = SphereTerrainGridVertexHeight(layer, c);
    const float h11 = SphereTerrainGridVertexHeight(layer, c + int2(1, 1));
    if (f.x >= f.y)
    {
        const float h10 = SphereTerrainGridVertexHeight(layer, c + int2(1, 0));
        return h00 + f.x * (h10 - h00) + f.y * (h11 - h10);
    }
    const float h01 = SphereTerrainGridVertexHeight(layer, c + int2(0, 1));
    return h00 + f.y * (h01 - h00) + f.x * (h11 - h01);
}

// Decodes a grid vertex index: the surface's 33 x 33 row-major, then one skirt vertex under every
// edge vertex, edge by edge (NegU, PosU, NegV, PosV). w is 1 for a skirt vertex.
int3 SphereTerrainGridVertex(uint index)
{
    if (index < SphereTerrainSurfaceVertices)
    {
        return int3(int(index % SphereTerrainGridRow), int(index / SphereTerrainGridRow), 0);
    }
    const uint skirt = index - SphereTerrainSurfaceVertices;
    const uint edge = skirt / SphereTerrainGridRow;
    const int k = int(skirt % SphereTerrainGridRow);
    const int last = int(SphereTerrainGridQuads);
    if (edge == 0u)
    {
        return int3(0, k, 1);
    }
    if (edge == 1u)
    {
        return int3(last, k, 1);
    }
    if (edge == 2u)
    {
        return int3(k, 0, 1);
    }
    return int3(k, last, 1);
}

// A tile-grid coordinate on its parent's grid.
float2 SphereTerrainParentGrid(float2 grid, uint quadrant)
{
    const float2 offset = float2(float(quadrant & 1u), float((quadrant >> 1) & 1u));
    return offset * float(SphereTerrainGridQuads / 2u) + grid * 0.5;
}

// A grid coordinate's texel coordinate in [0, 1] across a tile channel at full resolution.
float2 SphereTerrainGridUV(float2 grid)
{
    return (float(SphereTerrainGridBorder) + grid * float(SphereTerrainGridTexelStep) + 0.5) /
           float(SphereTerrainTexels);
}

// One evaluated terrain vertex, for either stage.
struct SphereTerrainVertex
{
    // Position relative to the eye, unscaled, morphed, with the skirt hung.
    float3 Position;
    // Last frame's position, relative to this frame's camera, in last frame's scene space.
    float3 PrevPosition;
    // The sphere's vertical.
    float3 Vertical;
    // The tile parameters of the morphed vertex.
    float2 St;
    // The corner weights at St.
    float4 Weights;
    // The morphed vertex's grid coordinate on the parent's grid.
    float2 ParentGrid;
    // The height above the radius the vertex stands at.
    float Height;
    // The morph factor.
    float Morph;
    // The record index.
    uint Record;
};

// Evaluates a terrain vertex: placed unmorphed to measure its eye distance, morphed by CDLOD's rule
// (odd grid coordinates slide toward the even one below, and the height blends from the tile's own
// grid to its parent's), placed again, and hung by the skirt depth when it is a skirt vertex. A
// level layer (surface > 0) stands at the layer's constant height and reads no height. A vertex
// with no morph, most of every tile, reads no further height and is placed once.
SphereTerrainVertex SphereTerrainEvaluate(uint vertexIndex, uint listIndex, uint surface)
{
    SphereTerrainVertex v;
    v.Record = g_TerrainList[listIndex];
    const SphereTerrainRecord r = g_TerrainRecords[v.Record];
    const int3 decoded = SphereTerrainGridVertex(vertexIndex);
    const int2 grid = decoded.xy;
    const uint layer = r.Layers.x;
    const float quads = float(SphereTerrainGridQuads);

    const bool layered = surface > 0u;
    const float layerHeight = surface == 1u ? r.Heights.x : r.Heights.y;
    const float gridHeight = layered ? layerHeight : SphereTerrainGridVertexHeight(layer, grid);
    v.St = float2(grid) / quads;
    v.Weights = SphereTerrainCornerWeights(r, v.St);
    SphereTerrainPlacement placed = SphereTerrainPlace(r, v.Weights, gridHeight);
    const float k = SphereTerrainMorphFactor(length(placed.Position), r.Morph);

    v.ParentGrid = SphereTerrainParentGrid(float2(grid), r.Layers.z);
    float height = gridHeight;
    // Coherent across a tile's interior: the morph is zero short of the morph start.
    if (k > 0.0)
    {
        const int2 odd = grid & int2(1);
        const float2 slid = float2(grid) - float2(odd) * k;
        v.ParentGrid = SphereTerrainParentGrid(slid, r.Layers.z);
        if (!layered)
        {
            const float own = lerp(gridHeight, SphereTerrainGridVertexHeight(layer, grid - odd), k);
            const float parent = SphereTerrainGridHeight(r.Layers.y, v.ParentGrid);
            height = lerp(own, parent, k);
        }
        v.St = slid / quads;
        v.Weights = SphereTerrainCornerWeights(r, v.St);
        placed = SphereTerrainPlace(r, v.Weights, height);
    }

    v.Vertical = placed.Vertical;
    v.Position = placed.Position;
    v.PrevPosition = SphereTerrainPlacePrevious(r, v.Weights, height, placed.Vertical);
    if (decoded.z != 0)
    {
        v.Position -= placed.Vertical * r.Morph.w;
        v.PrevPosition -= placed.Vertical * r.Morph.w;
    }
    v.Height = height;
    v.Morph = k;
    return v;
}
