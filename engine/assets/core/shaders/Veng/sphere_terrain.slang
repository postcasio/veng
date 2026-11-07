// The sphere terrain material contract: what a material declaring "sphereTerrain" reads. Its vertex
// stage is the engine's surface_terrain.vert (a Surface material) or translucent_terrain.vert (a
// Translucent level layer, see Veng/sphere_terrain_translucent.slang), which both run
// SphereTerrainVertexStage below: it places each vertex of the shared grid precisely about the eye,
// morphs it toward its parent, and writes SphereTerrainFragmentInput: the ordinary surface
// interpolants (Surface), plus the tile's texel coordinates, the morph factor and the record. The
// fragment's fsMain takes SphereTerrainFragmentInput.
//
// - TerrainChannel(input, channel, sampler) samples a payload channel blended with the parent's by
//   the geometry's own morph, so at a coarse-fine edge the fine side shades as its parent, which is
//   the coarse neighbour: the shading has no seam where the geometry has none. TerrainChannelAt
//   does the same at caller-given coordinates, for a channel held at a divisor of the resolution.
//   Neither samples the parent where the fragment does not morph.
// - TerrainTexelSize gives a tile texel's size in metres, for detail added by footprint above the
//   tile's band; TerrainDirectionOffset and TerrainCenterHigh / TerrainCenterLow a fragment's
//   body-fixed direction to full precision.
// - TerrainTileData / TerrainParentTileData read the generator's per-tile words.
// - TerrainTrueViewDepth and TerrainTrueDeviceDepth are the unscaled geometry's view and device
//   depth, interpolated: a terrain drawn at a scale about the eye returns GBufferDepthOutput with
//   one of them (or a curve of its own) to stand at its true depth.
// - TerrainSurface tells a terrain's own surface (0) from its level layers (1 and up), and
//   TerrainLayerHeight a layer's height.
// - TerrainTileLevel and TerrainTileHeld give the fragment's tile's quadtree level and whether the
//   selection held it whole; TerrainTileOverlay paints them over a colour when the view asks for
//   the tile overlay (SceneView::SphereTerrainTiles), and returns the colour unchanged otherwise.
#pragma once

#include "surface.slang"
#include "sphere_terrain_draw.slang"

struct SphereTerrainFragmentInput
{
    // The ordinary surface interpolants. v_UV is the tile's texel coordinate, v_WorldNormal the
    // sphere's vertical, v_ObjectPosition the body-fixed offset from the tile's centre in metres,
    // and v_CurClip the unscaled geometry's unjittered clip position.
    SurfaceFragmentInput Surface;
    // The parent's texel coordinate (xy).
    float2 v_TerrainParentUV : TEXCOORD7;
    // The morph factor: 0 the tile's own shape, 1 its parent's.
    float v_TerrainMorph : TEXCOORD8;
    // The record index (x) and the surface drawn (y): 0 the terrain, 1 + l level layer l.
    nointerpolation uint2 v_TerrainTile : TEXCOORD9;
};

// The terrain vertex stages' body. It reads no vertex attributes: the index names a vertex of the
// shared grid, the instance index names an entry of the view's tile list (the draw's firstInstance is
// the list's start), and the entry names the tile's record. The push block's InstanceDraw is the
// surface's DrawData record (its material) and InstanceBuffer the surface drawn (0 the terrain, 1 + l
// its level layer l).
//
// The vertex is placed about the eye at true size, then scaled about the eye by the record's scale
// for the rasterized position alone: the motion vector and the true depth the fragment can write are
// the unscaled geometry's, so a scale about the eye, which moves nothing on screen, changes nothing
// but the rasterized depth.
SphereTerrainFragmentInput SphereTerrainVertexStage(uint vertexIndex, uint listIndex)
{
    const uint surface = g_PC.InstanceBuffer;
    const SphereTerrainVertex v = SphereTerrainEvaluate(vertexIndex, listIndex, surface);
    const SphereTerrainRecord r = g_TerrainRecords[v.Record];
    const DrawData draw = LoadDrawData(g_PC.InstanceDraw);
    const ViewConstants view = LoadViewConstants(g_PC.ViewConstantsIndex);
    const float scale = r.ChordsB.w;

    const float3 camera = view.CameraPosition.xyz;
    const float3 rotated = mul((float3x3)view.View, v.Position * scale);

    SphereTerrainFragmentInput output;
    output.Surface.sv_position = mul(view.Proj, float4(rotated, 1.0));
    output.Surface.v_CurClip = mul(view.CurViewProj, float4(camera + v.Position, 1.0));
    output.Surface.v_PrevClip = mul(view.PrevViewProj, float4(camera + v.PrevPosition, 1.0));
    output.Surface.v_UV = SphereTerrainGridUV(v.St * float(SphereTerrainGridQuads));
    output.Surface.v_WorldNormal = v.Vertical;
    const float3 tangent = r.TangentU.xyz - v.Vertical * dot(v.Vertical, r.TangentU.xyz);
    output.Surface.v_WorldTangent = float4(normalize(tangent), 1.0);
    output.Surface.v_MaterialOffset = draw.MaterialOffset;
    output.Surface.v_CandidateId = g_PC.InstanceDraw;
    output.Surface.v_Instance = uint2(InstanceNone, 0u);
    const float3 offset = SphereTerrainTileOffset(r.Tile, v.Weights);
    output.Surface.v_ObjectPosition = r.ChordsB.z * offset + v.Height * (r.Tile.CenterHigh.xyz + offset);
    output.v_TerrainParentUV = SphereTerrainGridUV(v.ParentGrid);
    output.v_TerrainMorph = v.Morph;
    output.v_TerrainTile = uint2(v.Record, surface);
    return output;
}

// The fragment's tile record.
SphereTerrainRecord TerrainRecord(SphereTerrainFragmentInput input)
{
    return g_TerrainRecords[input.v_TerrainTile.x];
}

// One channel's array, sampled at a coordinate on a layer.
float4 TerrainSampleChannel(uint channel, SamplerState samplerState, float2 uv, uint layer)
{
    const float3 at = float3(uv, float(layer));
    switch (channel)
    {
    case 0u:
        return g_TerrainChannel0.SampleLevel(samplerState, at, 0.0);
    case 1u:
        return g_TerrainChannel1.SampleLevel(samplerState, at, 0.0);
    case 2u:
        return g_TerrainChannel2.SampleLevel(samplerState, at, 0.0);
    case 3u:
        return g_TerrainChannel3.SampleLevel(samplerState, at, 0.0);
    case 4u:
        return g_TerrainChannel4.SampleLevel(samplerState, at, 0.0);
    case 5u:
        return g_TerrainChannel5.SampleLevel(samplerState, at, 0.0);
    case 6u:
        return g_TerrainChannel6.SampleLevel(samplerState, at, 0.0);
    default:
        return g_TerrainChannel7.SampleLevel(samplerState, at, 0.0);
    }
}

// A payload channel at caller-given coordinates on the tile's layer (uv) and its parent's
// (parentUv), blended by the fragment's morph. For a channel held at a divisor of the tile's
// resolution, whose coordinates differ from the full-resolution v_UV and v_TerrainParentUV. The
// parent is sampled only where the fragment morphs, which is coherent across a tile's interior
// (zero short of its morph start), so most fragments take one sample.
float4 TerrainChannelAt(SphereTerrainFragmentInput input, uint channel, SamplerState samplerState,
                        float2 uv, float2 parentUv)
{
    const SphereTerrainRecord r = TerrainRecord(input);
    const float4 own = TerrainSampleChannel(channel, samplerState, uv, r.Layers.x);
    if (input.v_TerrainMorph <= 0.0)
    {
        return own;
    }
    const float4 parent = TerrainSampleChannel(channel, samplerState, parentUv, r.Layers.y);
    return lerp(own, parent, input.v_TerrainMorph);
}

// A payload channel at the fragment: the tile's sample blended with the parent's by the morph,
// the parent sampled only where the fragment morphs (see TerrainChannelAt).
float4 TerrainChannel(SphereTerrainFragmentInput input, uint channel, SamplerState samplerState)
{
    return TerrainChannelAt(input, channel, samplerState, input.Surface.v_UV,
                            input.v_TerrainParentUV);
}

// The morph factor at the fragment.
float TerrainMorph(SphereTerrainFragmentInput input)
{
    return input.v_TerrainMorph;
}

// The surface drawn: 0 the terrain's own, 1 + l its level layer l.
uint TerrainSurface(SphereTerrainFragmentInput input)
{
    return input.v_TerrainTile.y;
}

// A level layer's height above the radius, in metres.
float TerrainLayerHeight(SphereTerrainFragmentInput input, uint layer)
{
    const SphereTerrainRecord r = TerrainRecord(input);
    return layer == 0u ? r.Heights.x : r.Heights.y;
}

// The size of one of the tile's texels, in metres.
float TerrainTexelSize(SphereTerrainFragmentInput input)
{
    return TerrainRecord(input).Heights.z;
}

// The tile parameters of the fragment.
float2 TerrainTileParameter(SphereTerrainFragmentInput input)
{
    return (input.Surface.v_UV * float(SphereTerrainTexels) - 0.5 - float(SphereTerrainGridBorder)) /
           float(SphereTerrainGridQuads * SphereTerrainGridTexelStep);
}

// The fragment's body-fixed direction relative to the tile's centre, precise at any depth: add it
// to TerrainCenterLow, then TerrainCenterHigh, as late as the work allows.
float3 TerrainDirectionOffset(SphereTerrainFragmentInput input)
{
    return SphereTerrainTileOffset(TerrainRecord(input).Tile, TerrainTileParameter(input));
}

// The tile's centre direction rounded to f32.
float3 TerrainCenterHigh(SphereTerrainFragmentInput input)
{
    return TerrainRecord(input).Tile.CenterHigh.xyz;
}

// The tile's centre direction's residual below f32.
float3 TerrainCenterLow(SphereTerrainFragmentInput input)
{
    return TerrainRecord(input).Tile.CenterLow.xyz;
}

// The generator's word i for the fragment's tile.
uint TerrainTileData(SphereTerrainFragmentInput input, uint i)
{
    return TerrainRecord(input).Data[i];
}

// The generator's word i for the fragment's tile's parent.
uint TerrainParentTileData(SphereTerrainFragmentInput input, uint i)
{
    return TerrainRecord(input).ParentData[i];
}

// The unscaled geometry's view depth at the fragment, in metres.
float TerrainTrueViewDepth(SphereTerrainFragmentInput input)
{
    return input.Surface.v_CurClip.w;
}

// The unscaled geometry's device depth at the fragment under the view's projection: what the
// terrain would have rasterized at scale one. Representable at any distance under an infinite
// far plane.
float TerrainTrueDeviceDepth(SphereTerrainFragmentInput input)
{
    return input.Surface.v_CurClip.z / input.Surface.v_CurClip.w;
}

// The fragment's tile's quadtree level.
uint TerrainTileLevel(SphereTerrainFragmentInput input)
{
    return SphereTerrainTileLevel(TerrainRecord(input).Tile);
}

// Whether the selection held the fragment's tile whole: it wanted to split and was not.
bool TerrainTileHeld(SphereTerrainFragmentInput input)
{
    return (TerrainRecord(input).Layers.w & SphereTerrainRecordHeld) != 0u;
}

// Whether the view asks for the tile overlay.
bool TerrainTileOverlayRequested(SphereTerrainFragmentInput input)
{
    return (TerrainRecord(input).Layers.w & SphereTerrainRecordTileOverlay) != 0u;
}

// A quadtree level's overlay colour. Successive levels are a golden-ratio turn of hue apart, so
// neighbouring levels always contrast.
float3 SphereTerrainLevelColor(uint level)
{
    const float hue = frac(0.08 + 0.618034 * float(level));
    const float3 pure = saturate(abs(frac(hue + float3(0.0, 2.0 / 3.0, 1.0 / 3.0)) * 6.0 - 3.0) - 1.0);
    return 0.85 * lerp(float3(1.0), pure, 0.75);
}

// The tile overlay over a colour, or the colour unchanged when the view does not ask for it.
//
// A tile is filled with its level's colour, striped with its parent level's as far as it has
// morphed toward its parent (solid at a full morph, where it is drawn as its parent), darkened
// where the selection held it whole, and outlined in near-black about a pixel wide on each side
// of its edges, so a seam between two tiles reads as one line. Skirts take their edge's outline.
// The colour is a fragment's surface colour before lighting, so the relief still reads through it.
float3 TerrainTileOverlay(SphereTerrainFragmentInput input, float3 color)
{
    // Derivatives taken ahead of the branch, in uniform control flow.
    const float2 st = TerrainTileParameter(input);
    const float2 pixel = max(fwidth(st), float2(1e-7));
    if (!TerrainTileOverlayRequested(input))
    {
        return color;
    }

    const uint level = TerrainTileLevel(input);
    const float3 own = SphereTerrainLevelColor(level);
    const float3 parent = level > 0u ? SphereTerrainLevelColor(level - 1u) : own;
    const float morph = TerrainMorph(input);
    const float stripeCycles = 8.0;
    const float stripe = frac((st.x + st.y) * stripeCycles);
    const float stripeBlur = max((pixel.x + pixel.y) * stripeCycles, 1e-4);
    const float striped =
        morph <= 0.0 ? 0.0 : (morph >= 1.0 ? 1.0 : saturate((morph - stripe) / stripeBlur + 0.5));
    float3 fill = lerp(own, parent, striped);
    if (TerrainTileHeld(input))
    {
        fill *= 0.55;
    }

    const float2 edgePixels = min(st, 1.0 - st) / pixel;
    const float outline = 1.0 - saturate(min(edgePixels.x, edgePixels.y) - 0.5);
    return lerp(fill, float3(0.02), outline);
}
