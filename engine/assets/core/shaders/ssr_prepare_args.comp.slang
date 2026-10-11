// Stochastic SSR dispatch arguments: turns the classifier's ray and tile totals into the indirect
// dispatch records of the passes consuming its lists — one 64-wide group per 64 rays for the
// intersect, one group per tile for the denoiser — and rotates the counters, so each total sits in
// its count slot for those passes and the append slots start the next frame at zero
// (Veng/ssr_ray_list.slang). One invocation.
//
// The argument layout and the counter rotation follow the AMD FidelityFX SDK's stochastic
// screen-space reflections, under this licence:
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
#include "Veng/ssr_ray_list.slang"

// Set 0 is reserved for the bindless registry; the counters and the arguments bind on set 3.
[[vk::binding(0, 3)]] globallycoherent RWStructuredBuffer<uint> g_Counters;
[[vk::binding(1, 3)]] RWStructuredBuffer<uint> g_Arguments;

[shader("compute")]
[numthreads(1, 1, 1)]
void csMain()
{
    uint rays = g_Counters[SsrRayAppendCounter];
    g_Arguments[SsrIntersectArguments + 0u] = (rays + 63u) / 64u;
    g_Arguments[SsrIntersectArguments + 1u] = 1u;
    g_Arguments[SsrIntersectArguments + 2u] = 1u;
    g_Counters[SsrRayAppendCounter] = 0u;
    g_Counters[SsrRayCountCounter] = rays;

    uint tiles = g_Counters[SsrTileAppendCounter];
    g_Arguments[SsrDenoiseArguments + 0u] = tiles;
    g_Arguments[SsrDenoiseArguments + 1u] = 1u;
    g_Arguments[SsrDenoiseArguments + 2u] = 1u;
    g_Counters[SsrTileAppendCounter] = 0u;
    g_Counters[SsrTileCountCounter] = tiles;
}
