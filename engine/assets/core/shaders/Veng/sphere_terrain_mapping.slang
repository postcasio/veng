// The cube-sphere tile mapping, precise at any depth: the device half of Veng/Math/CubeSphere.h.
//
// A sphere terrain tile is a rectangle in a cube face's equal-angle coordinates. A texel's
// direction is reached in two parts: the tile's centre, which only double precision holds to the
// metre on a large sphere and which the host supplies split into an f32 value and its residual, and
// an offset from that centre built from small differences only, accurate to the f32 rounding of
// the tile's own size. A kernel working at depth keeps the two apart for as long as it can.
//
// Every function here is bit-for-bit the C++ twin's arithmetic (CubeSphere::TileFractionF32,
// CubeSphereTileOffsetF32), and none calls a transcendental intrinsic: the sine and cosine are
// fixed polynomials, so the result does not depend on a device's fast-math mode. Bindings-free, so
// a consumer's tile kernel and the engine's draw stages include it alike.
#pragma once

// One tile's placement; mirrors Veng::Renderer::SphereTerrainTileConstants (128 bytes).
struct SphereTerrainTileConstants
{
    // The centre direction rounded to f32 (xyz) and the sphere's radius in metres (w).
    float4 CenterHigh;
    // The centre direction's residual below f32 (xyz); w is unused.
    float4 CenterLow;
    // Per corner (index sx + 2 sy): the offset from the centre direction (xyz), the cube-point norm (w).
    float4 Corners[4];
    // The cube-plane starts and warp spans: (x0u, dtheta, x0v, dtheta).
    float4 Warp;
    // x: face in bits 0-7, level in 8-15; y: column; z: row; w: TileTexels | Border << 8 | Cells << 16.
    uint4 Tile;
};

// The tile's face index.
uint SphereTerrainTileFace(SphereTerrainTileConstants tile)
{
    return tile.Tile.x & 0xFFu;
}

// The tile's quadtree level.
uint SphereTerrainTileLevel(SphereTerrainTileConstants tile)
{
    return (tile.Tile.x >> 8) & 0xFFu;
}

// The texels a side of a full-resolution channel.
uint SphereTerrainTileTexels(SphereTerrainTileConstants tile)
{
    return tile.Tile.w & 0xFFu;
}

// The border texels on each side.
uint SphereTerrainTileBorder(SphereTerrainTileConstants tile)
{
    return (tile.Tile.w >> 8) & 0xFFu;
}

// The cells across the tile's interior.
uint SphereTerrainTileCells(SphereTerrainTileConstants tile)
{
    return tile.Tile.w >> 16;
}

// The polynomial sine, accurate to f32 on |x| <= 1.7 (Taylor to x^13).
float SphereTerrainSin(float x)
{
    precise float x2 = x * x;
    precise float p = 1.0 / 6227020800.0;
    p = p * x2 - 1.0 / 39916800.0;
    p = p * x2 + 1.0 / 362880.0;
    p = p * x2 - 1.0 / 5040.0;
    p = p * x2 + 1.0 / 120.0;
    p = p * x2 - 1.0 / 6.0;
    precise float result = x + x * x2 * p;
    return result;
}

// The polynomial cosine, accurate to f32 on |x| <= 1.7 (Taylor to x^14).
float SphereTerrainCos(float x)
{
    precise float x2 = x * x;
    precise float p = -1.0 / 87178291200.0;
    p = p * x2 + 1.0 / 479001600.0;
    p = p * x2 - 1.0 / 3628800.0;
    p = p * x2 + 1.0 / 40320.0;
    p = p * x2 - 1.0 / 720.0;
    p = p * x2 + 1.0 / 24.0;
    p = p * x2 - 0.5;
    precise float result = 1.0 + x2 * p;
    return result;
}

float SphereTerrainShifted(float c, float s, float x0)
{
    precise float result = c - x0 * s;
    return result;
}

// The face-coordinate fraction across a tile at tile parameter s:
// S(s dtheta)(C(dtheta) - x0 S(dtheta)) / (S(dtheta)(C(s dtheta) - x0 S(s dtheta))).
// Exactly 0 at s = 0 and 1 at s = 1, and strictly increasing; it never subtracts two nearly equal
// tangents, so it holds f32 relative precision at the deepest level.
float SphereTerrainTileFraction(float s, float x0, float dtheta)
{
    precise float a = s * dtheta;
    precise float sinA = SphereTerrainSin(a);
    precise float sinD = SphereTerrainSin(dtheta);
    precise float result = (sinA * SphereTerrainShifted(SphereTerrainCos(dtheta), sinD, x0)) /
                           (sinD * SphereTerrainShifted(SphereTerrainCos(a), sinA, x0));
    return result;
}

// The fraction from the tile's arc terms (S(dtheta), C(dtheta) - x0 S(dtheta)), computed once per
// tile on the host (CubeSphere::TileArcF32): the same quotient, exactly 0 at s = 0 and 1 at s = 1.
float SphereTerrainTileFractionArc(float s, float x0, float dtheta, float2 arc)
{
    if (s == 0.0 || s == 1.0)
    {
        return s;
    }
    precise float a = s * dtheta;
    precise float sinA = SphereTerrainSin(a);
    precise float result = (sinA * arc.y) / (arc.x * SphereTerrainShifted(SphereTerrainCos(a), sinA, x0));
    return result;
}

// The direction offset from corner weights w_i = b_i |Q_i| / sum b_j |Q_j| already computed for the
// tile parameters: delta = sum w_i (d_i - d_c), then as SphereTerrainTileOffset.
float3 SphereTerrainTileOffset(SphereTerrainTileConstants tile, float4 weights)
{
    precise float3 delta = weights.x * tile.Corners[0].xyz + weights.y * tile.Corners[1].xyz +
                           weights.z * tile.Corners[2].xyz + weights.w * tile.Corners[3].xyz;
    float3 center = tile.CenterHigh.xyz;
    precise float shortfall = 2.0 * dot(center, delta) + dot(delta, delta);
    precise float m = sqrt(1.0 + shortfall);
    precise float3 offset = (delta - center * (shortfall / (1.0 + m))) / m;
    return offset;
}

// A direction relative to the tile's centre at tile parameters st (0 and 1 the tile's edges):
// with bilinear weights of the warped fractions, w_i = b_i |Q_i| / sum b_j |Q_j| and
// delta = sum w_i (d_i - d_c), the chord point is m = d_c + delta and its direction's offset is
// (delta - d_c (2 d_c.delta + |delta|^2) / (1 + |m|)) / |m|.
float3 SphereTerrainTileOffset(SphereTerrainTileConstants tile, float2 st)
{
    precise float au = SphereTerrainTileFraction(st.x, tile.Warp.x, tile.Warp.y);
    precise float av = SphereTerrainTileFraction(st.y, tile.Warp.z, tile.Warp.w);
    precise float4 beta = float4((1.0 - au) * (1.0 - av), au * (1.0 - av), (1.0 - au) * av, au * av) *
                          float4(tile.Corners[0].w, tile.Corners[1].w, tile.Corners[2].w, tile.Corners[3].w);
    precise float total = beta.x + beta.y + beta.z + beta.w;
    precise float3 delta = (beta.x * tile.Corners[0].xyz + beta.y * tile.Corners[1].xyz +
                            beta.z * tile.Corners[2].xyz + beta.w * tile.Corners[3].xyz) /
                           total;
    float3 center = tile.CenterHigh.xyz;
    precise float shortfall = 2.0 * dot(center, delta) + dot(delta, delta);
    precise float m = sqrt(1.0 + shortfall);
    precise float3 offset = (delta - center * (shortfall / (1.0 + m))) / m;
    return offset;
}

// The tile parameter of a channel texel: texel k of a channel at divisor n sits at
// s = (k n - Border) / Cells, so the border texels fall just outside [0, 1].
float2 SphereTerrainTexelParameter(SphereTerrainTileConstants tile, uint2 texel, uint divisor)
{
    return (float2(texel * divisor) - float(SphereTerrainTileBorder(tile))) /
           float(SphereTerrainTileCells(tile));
}

// A texel's direction offset from the tile's centre, precise at any depth.
float3 SphereTerrainTexelOffset(SphereTerrainTileConstants tile, uint2 texel, uint divisor)
{
    return SphereTerrainTileOffset(tile, SphereTerrainTexelParameter(tile, texel, divisor));
}

// A texel's unit direction in f32: the centre plus its residual plus the offset. Rounded to f32 as
// a whole, it resolves about 6e-8 of the radius; a kernel needing more keeps the offset apart.
float3 SphereTerrainTexelDirection(SphereTerrainTileConstants tile, uint2 texel, uint divisor)
{
    return tile.CenterHigh.xyz + (tile.CenterLow.xyz + SphereTerrainTexelOffset(tile, texel, divisor));
}
