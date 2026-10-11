// Stochastic SSR denoiser, last pass: keeps what next frame's reprojection compares against. One
// invocation per pixel of the render extent, after the temporal resolve.
//
// - Every pixel's normal, GGX alpha and depth, and a glossy pixel's sample count, are copied into
//   their history targets.
// - A pixel that is not glossy has no reflection: its denoised output, variance and sample count
//   are zero. The resolve writes only the tiles holding a glossy pixel, so this is also what clears
//   every other tile.
//
// The history the denoiser keeps follows the AMD FidelityFX SDK's reflections denoiser, under this
// licence:
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
[[vk::binding(0, 0)]] Texture2D g_Textures[];

// Set 0 is reserved for the bindless registry; the history targets bind on set 3.
[[vk::binding(0, 3)]] [format("rgba16f")] RWTexture2D<float4> g_NormalHistory;
[[vk::binding(1, 3)]] [format("r8")] RWTexture2D<float> g_AlphaHistory;
[[vk::binding(2, 3)]] [format("r32f")] RWTexture2D<float> g_DepthHistory;
[[vk::binding(3, 3)]] [format("r16f")] RWTexture2D<float> g_SampleCountHistory;
[[vk::binding(4, 3)]] [format("rgba16f")] RWTexture2D<float4> g_Output;
[[vk::binding(5, 3)]] [format("r16f")] RWTexture2D<float> g_Variance;

struct PushConstants
{
    uint NormalTexture;       // G1 world-space normal
    uint AlphaTexture;        // the classifier's GGX alpha
    uint DepthTexture;        // the nearest-depth pyramid, level 0 the depth
    uint SampleCountTexture;  // the reprojection's sample counts
    uint2 Extent;             // render extent in pixels
    float MaxAlpha;           // glossy below this alpha
    uint Pad0;
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint3 dispatch : SV_DispatchThreadID)
{
    int2 pixel = int2(dispatch.xy);
    if (any(pixel >= int2(g_PC.Extent)))
        return;
    int3 texel = int3(pixel, 0);
    float alpha = g_Textures[g_PC.AlphaTexture].Load(texel).r;
    bool glossy = alpha < g_PC.MaxAlpha;
    g_NormalHistory[pixel] = g_Textures[g_PC.NormalTexture].Load(texel);
    g_AlphaHistory[pixel] = alpha;
    g_DepthHistory[pixel] = g_Textures[g_PC.DepthTexture].Load(texel).r;
    g_SampleCountHistory[pixel] = glossy ? g_Textures[g_PC.SampleCountTexture].Load(texel).r : 0.0;
    if (!glossy)
    {
        g_Output[pixel] = float4(0.0, 0.0, 0.0, 0.0);
        g_Variance[pixel] = 0.0;
    }
}
