// The stochastic reflection chain's work lists: the ray list the classifier compacts the pixels that
// trace into, the denoiser tile list it compacts the 8x8 tiles holding a glossy pixel into, the four
// counters both are appended through, and the dispatch arguments the passes consuming them run by.
//
// A ray is packed as its pixel (x in 15 bits, y in 14) and three copy flags in bits 29-31, naming
// which of its quad neighbours trace nothing and take this ray's result. A tile is packed as its
// origin pixel, x | y << 16.
//
// The packing and the counter rotation follow the AMD FidelityFX SDK's stochastic screen-space
// reflections, under this licence:
//
// Copyright (C) 2024 Advanced Micro Devices, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy of this software and
// associated documentation files (the "Software"), to deal in the Software without restriction,
// including without limitation the rights to use, copy, modify, merge, publish, distribute,
// sublicense, and/or sell copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions:
//
// The above copyright notice and this permission notice shall be included in all copies or
// substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR IMPLIED, INCLUDING BUT
// NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY, FITNESS FOR A PARTICULAR PURPOSE AND
// NONINFRINGEMENT. IN NO EVENT SHALL THE AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM,
// DAMAGES OR OTHER LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM, OUT
// OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN THE SOFTWARE.

#ifndef VENG_SSR_RAY_LIST_SLANG
#define VENG_SSR_RAY_LIST_SLANG

// The counter slots. The classifier appends to the two append slots; the argument pass moves each
// total into its count slot, which the consuming passes read, and zeroes the append slot for the
// next frame.
static const uint SsrRayAppendCounter = 0u;
static const uint SsrRayCountCounter = 1u;
static const uint SsrTileAppendCounter = 2u;
static const uint SsrTileCountCounter = 3u;

// The dispatch arguments' records, in words: the intersect's (one 64-wide group per 64 rays), then
// the denoiser's (one group per tile).
static const uint SsrIntersectArguments = 0u;
static const uint SsrDenoiseArguments = 3u;

// The largest pixel coordinates a packed ray can name.
static const uint SsrRayMaxX = 32767u;
static const uint SsrRayMaxY = 16383u;

// One entry of the ray list, unpacked.
struct SsrRay
{
    uint2 Pixel;
    bool CopyHorizontal;  // the quad neighbour across x takes this ray's result
    bool CopyVertical;    // the quad neighbour across y takes it
    bool CopyDiagonal;    // the quad neighbour across both takes it
};

uint SsrPackRay(uint2 pixel, bool copyHorizontal, bool copyVertical, bool copyDiagonal)
{
    return (copyDiagonal ? 1u << 31 : 0u) | (copyVertical ? 1u << 30 : 0u) |
           (copyHorizontal ? 1u << 29 : 0u) | ((pixel.y & SsrRayMaxY) << 15) | (pixel.x & SsrRayMaxX);
}

SsrRay SsrUnpackRay(uint packed)
{
    SsrRay ray;
    ray.Pixel = uint2(packed & SsrRayMaxX, (packed >> 15) & SsrRayMaxY);
    ray.CopyHorizontal = ((packed >> 29) & 1u) != 0u;
    ray.CopyVertical = ((packed >> 30) & 1u) != 0u;
    ray.CopyDiagonal = ((packed >> 31) & 1u) != 0u;
    return ray;
}

uint SsrPackTile(uint2 origin)
{
    return ((origin.y & 0xFFFFu) << 16) | (origin.x & 0xFFFFu);
}

uint2 SsrUnpackTile(uint packed)
{
    return uint2(packed & 0xFFFFu, packed >> 16);
}

#endif
