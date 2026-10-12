// Screen-space reflections: one level of the depth hierarchy the intersection marches. Ports the
// contents of ffx_sssr_depth_downsample.h (FidelityFX SDK v1.1.4) one level per dispatch: level 0
// is a copy of the depth target, and each deeper level holds, per texel, the nearest of its parent
// level's 2×2 texels. The engine's depth is reverse-Z (near 1, far 0), so the nearest is the
// largest, the reference's inverted-depth max (SpdReduce4).
//
// A child past the parent level's extent contributes 0, the value the reference's out-of-bounds
// load returns; under max it changes nothing, since every depth is at least 0. A parent level of
// odd size therefore drops its last row or column from the level below it, as the reference's
// strict 2×2 reduction does.
//
// The source — the depth target for level 0, the previous level otherwise — is read through the
// bindless set-0 textures; the destination level is this pass's set 3.
//
// This file is part of the FidelityFX SDK.
//
// Copyright (C) 2024 Advanced Micro Devices, Inc.
//
// Permission is hereby granted, free of charge, to any person obtaining a copy
// of this software and associated documentation files(the "Software"), to deal
// in the Software without restriction, including without limitation the rights
// to use, copy, modify, merge, publish, distribute, sublicense, and /or sell
// copies of the Software, and to permit persons to whom the Software is
// furnished to do so, subject to the following conditions :
//
// The above copyright notice and this permission notice shall be included in
// all copies or substantial portions of the Software.
//
// THE SOFTWARE IS PROVIDED "AS IS", WITHOUT WARRANTY OF ANY KIND, EXPRESS OR
// IMPLIED, INCLUDING BUT NOT LIMITED TO THE WARRANTIES OF MERCHANTABILITY,
// FITNESS FOR A PARTICULAR PURPOSE AND NONINFRINGEMENT. IN NO EVENT SHALL THE
// AUTHORS OR COPYRIGHT HOLDERS BE LIABLE FOR ANY CLAIM, DAMAGES OR OTHER
// LIABILITY, WHETHER IN AN ACTION OF CONTRACT, TORT OR OTHERWISE, ARISING FROM,
// OUT OF OR IN CONNECTION WITH THE SOFTWARE OR THE USE OR OTHER DEALINGS IN
// THE SOFTWARE.

[[vk::binding(0, 0)]] Texture2D g_Textures[];

[[vk::binding(0, 3)]] [format("r32f")] RWTexture2D<float> g_DepthHierarchy;

struct PushConstants
{
    uint SourceTexture; // bindless index of the depth target (level 0) or the parent level
    uint CopiesSource;  // 1 for level 0, which copies its source; 0 for a 2×2 reduction
    uint2 DestExtent;   // this level's extent in texels
    uint2 SourceExtent; // the source's extent in texels
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

float LoadSource(int2 coordinate)
{
    if (any(coordinate >= int2(g_PC.SourceExtent)))
    {
        return 0.0;
    }
    return g_Textures[NonUniformResourceIndex(g_PC.SourceTexture)].Load(int3(coordinate, 0)).r;
}

float SpdReduce4(float v0, float v1, float v2, float v3)
{
    return max(max(v0, v1), max(v2, v3));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint3 dispatchThreadId : SV_DispatchThreadID)
{
    if (any(dispatchThreadId.xy >= g_PC.DestExtent))
    {
        return;
    }
    int2 pixel = int2(dispatchThreadId.xy);

    if (g_PC.CopiesSource != 0u)
    {
        g_DepthHierarchy[pixel] = LoadSource(pixel);
        return;
    }

    int2 base = pixel * 2;
    g_DepthHierarchy[pixel] = SpdReduce4(LoadSource(base), LoadSource(base + int2(1, 0)),
                                         LoadSource(base + int2(0, 1)), LoadSource(base + int2(1, 1)));
}
