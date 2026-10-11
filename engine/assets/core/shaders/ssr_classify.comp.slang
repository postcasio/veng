// Stochastic SSR tile classification: decides which pixels trace a reflection ray this frame,
// compacts them into the ray list, lists every 8x8 tile holding a glossy pixel for the denoiser,
// extracts each pixel's GGX α, and clears the radiance and ray-length targets the intersect then
// fills. A tile it does not list takes a zero average radiance, so the denoiser's bilinear reads of
// the tile averages never meet a stale one.
//
// One 64x1 workgroup covers one 8x8 tile, its local indices remapped so each 2x2 quad sits in four
// consecutive ones (local index ^ 1, ^ 2, ^ 3 are the quad's horizontal, vertical and diagonal
// neighbours).
//
// - α is the specular-anti-aliasing-widened perceptual roughness squared (ssr_surface.slang), the
//   lobe the lighting core shades the pixel with. A pixel reflects when it has a surface (reverse-Z
//   depth above 0) and a normal; it is glossy below MaxAlpha and a mirror below SsrMirrorAlpha. A
//   pixel that does not reflect stores α 1, so nothing downstream takes it for glossy.
// - A glossy pixel traces when it is a mirror, when it is its copy group's base ray, or when the
//   denoiser's last-frame variance there is above the threshold. The density sets the groups and
//   their bases: One, the quad, based at its even-even pixel; Two, each row's pair, based on the
//   quad's diagonal; Four, every pixel alone.
// - A group whose designated base is not glossy — background, or past the cutoff — is based at its
//   first glossy pixel instead, so a quad cut by a silhouette or a roughness edge still traces.
// - A glossy, non-mirror pixel tracing nothing is named by a copy flag on its group's base ray.
//   Each pixel's state is exchanged through workgroup memory rather than across subgroup lanes,
//   whose order relative to the local index Vulkan does not promise.
// - The ray list is compacted per subgroup: a prefix count of the lanes tracing, one atomic per
//   subgroup on the append counter, and the base broadcast from the first lane. That is correct at
//   any subgroup width and lane order.
//
// The classification follows the AMD FidelityFX SDK's stochastic screen-space reflections, under
// this licence:
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
#include "ssr_surface.slang"
#include "Veng/ssr_ray_list.slang"
#include "Veng/ssr_sample.slang"

[[vk::binding(0, 0)]] Texture2D g_Textures[];

// Set 0 is reserved for the bindless registry; the chain's lists and storage targets bind on set 3.
[[vk::binding(0, 3)]] RWStructuredBuffer<uint> g_RayList;
[[vk::binding(1, 3)]] RWStructuredBuffer<uint> g_TileList;
[[vk::binding(2, 3)]] globallycoherent RWStructuredBuffer<uint> g_Counters;
[[vk::binding(3, 3)]] [format("rgba16f")] RWTexture2D<float4> g_Radiance;
[[vk::binding(4, 3)]] [format("r8")] RWTexture2D<float> g_Alpha;
[[vk::binding(5, 3)]] [format("r16f")] RWTexture2D<float> g_RayLength;
[[vk::binding(6, 3)]] [format("rgba16f")] RWTexture2D<float4> g_Average;

struct PushConstants
{
    uint DepthTexture;        // depth attachment
    uint NormalTexture;       // G1 world-space normal
    uint OrmTexture;          // G2 packed occlusion/roughness/metallic
    uint VarianceTexture;     // the denoiser's last-frame variance, or NoVariance
    uint ViewConstantsIndex;  // current frame-in-flight's view-constants region
    uint SamplesPerQuad;      // 1, 2 or 4
    float MaxAlpha;           // glossy below this α: the squared perceptual roughness cutoff
    uint Pad0;
    uint2 Extent;             // render extent in pixels
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

// The VarianceTexture value naming no variance target: every pixel's variance reads 0.
static const uint NoVariance = 0xFFFFFFFFu;

// Variance-guided tracing: a glossy pixel whose quad's base ray stands in for it traces anyway when
// the denoiser's variance there is above the threshold.
static const bool VarianceGuidedTracing = true;
static const float VarianceThreshold = 0.0;

groupshared uint g_TileCount;
// Each pixel's state, by local index: PixelGlossy | PixelDenoised | PixelHighVariance.
groupshared uint g_PixelState[64];

static const uint PixelGlossy = 1u;        // reflective, below the roughness cutoff
static const uint PixelDenoised = 2u;      // glossy and not a mirror: the denoiser reconstructs it
static const uint PixelHighVariance = 4u;  // the denoiser's variance there is above the threshold

// Local index → pixel within the 8x8 tile, so each 2x2 quad holds four consecutive indices:
//     00 01 08 09 10 11 18 19
//     02 03 0a 0b 12 13 1a 1b
//     04 05 0c 0d 14 15 1c 1d
//     06 07 0e 0f 16 17 1e 1f
//     20 21 28 29 30 31 38 39
//     ...
uint2 RemapForQuads(uint index)
{
    uint x = (index & 1u) | (((index >> 2) & 7u) & ~1u);
    uint y = ((index >> 1) & 3u) | (((index >> 3) & 7u) & ~3u);
    return uint2(x, y);
}

bool HasState(uint index, uint state)
{
    return (g_PixelState[index] & state) != 0u;
}

// The local index of the base ray of `index`'s copy group at `samplesPerQuad`, or NoBase when no
// pixel of the group is glossy. A quad's local indices run (0,0), (1,0), (0,1), (1,1).
static const uint NoBase = 0xFFFFFFFFu;
uint GroupBase(uint index, uint samplesPerQuad)
{
    if (samplesPerQuad == 4u)
        return index;
    uint quad = index & ~3u;
    if (samplesPerQuad == 2u)
    {
        // The row pairs, based on the quad's diagonal.
        uint designated = (index & 2u) != 0u ? quad + 3u : quad;
        if (HasState(designated, PixelGlossy))
            return designated;
        return HasState(designated ^ 1u, PixelGlossy) ? designated ^ 1u : NoBase;
    }
    for (uint i = 0u; i < 4u; ++i)
    {
        if (HasState(quad + i, PixelGlossy))
            return quad + i;
    }
    return NoBase;
}

bool NeedsRay(uint index, uint samplesPerQuad)
{
    if (!HasState(index, PixelGlossy))
        return false;
    // A mirror always traces: the denoiser does not reconstruct it from its group.
    return !HasState(index, PixelDenoised) || GroupBase(index, samplesPerQuad) == index ||
           HasState(index, PixelHighVariance);
}

// A pixel the denoiser reconstructs but no ray of its own covers takes its group base's result.
bool RequiresCopy(uint index, uint samplesPerQuad)
{
    return HasState(index, PixelDenoised) && !NeedsRay(index, samplesPerQuad);
}

[shader("compute")]
[numthreads(64, 1, 1)]
void csMain(uint localIndex : SV_GroupIndex, uint3 group : SV_GroupID)
{
    if (localIndex == 0u)
        g_TileCount = 0u;

    uint2 pixel = group.xy * 8u + RemapForQuads(localIndex);
    bool inside = all(pixel < g_PC.Extent);

    Texture2D depthTexture = g_Textures[g_PC.DepthTexture];
    Texture2D normalTexture = g_Textures[g_PC.NormalTexture];
    bool reflective = false;
    float alpha = 1.0;
    if (inside)
    {
        int3 texel = int3(int2(pixel), 0);
        float depth = depthTexture.Load(texel).r;
        float3 normal = normalTexture.Load(texel).xyz;
        reflective = depth > 0.0 && length(normal) >= 1e-4;
        ViewConstants vc = LoadViewConstants(g_PC.ViewConstantsIndex);
        float roughness = SsrRoughness(vc, depthTexture, normalTexture,
                                       g_Textures[g_PC.OrmTexture], int2(pixel));
        alpha = roughness * roughness;
    }

    bool glossy = reflective && alpha < g_PC.MaxAlpha;
    bool mirror = alpha < SsrMirrorAlpha;
    bool needsDenoiser = glossy && !mirror;
    bool highVariance = false;
    if (VarianceGuidedTracing && needsDenoiser && g_PC.VarianceTexture != NoVariance)
        highVariance = g_Textures[g_PC.VarianceTexture].Load(int3(int2(pixel), 0)).r > VarianceThreshold;
    g_PixelState[localIndex] = (glossy ? PixelGlossy : 0u) | (needsDenoiser ? PixelDenoised : 0u) |
                               (highVariance ? PixelHighVariance : 0u);

    // Every pixel's state, and the tile count's clear, land before any is read.
    GroupMemoryBarrierWithGroupSync();

    if (glossy)
        InterlockedAdd(g_TileCount, 1u);

    uint samplesPerQuad = g_PC.SamplesPerQuad;
    bool needsRay = NeedsRay(localIndex, samplesPerQuad);
    bool baseRay = glossy && GroupBase(localIndex, samplesPerQuad) == localIndex;
    bool copyHorizontal = baseRay && samplesPerQuad != 4u && RequiresCopy(localIndex ^ 1u, samplesPerQuad);
    bool copyVertical = baseRay && samplesPerQuad == 1u && RequiresCopy(localIndex ^ 2u, samplesPerQuad);
    bool copyDiagonal = baseRay && samplesPerQuad == 1u && RequiresCopy(localIndex ^ 3u, samplesPerQuad);

    uint rayInWave = WavePrefixCountBits(needsRay);
    uint waveRays = WaveActiveCountBits(needsRay);
    uint waveBase = 0u;
    if (WaveIsFirstLane())
        InterlockedAdd(g_Counters[SsrRayAppendCounter], waveRays, waveBase);
    waveBase = WaveReadLaneFirst(waveBase);
    if (needsRay)
        g_RayList[waveBase + rayInWave] = SsrPackRay(pixel, copyHorizontal, copyVertical, copyDiagonal);

    if (inside)
    {
        g_Radiance[pixel] = float4(0.0, 0.0, 0.0, 0.0);
        g_RayLength[pixel] = 0.0;
        g_Alpha[pixel] = reflective ? alpha : 1.0;
    }

    // Every pixel has added to the tile count.
    GroupMemoryBarrierWithGroupSync();
    if (localIndex == 0u && g_TileCount > 0u)
    {
        uint tile;
        InterlockedAdd(g_Counters[SsrTileAppendCounter], 1u, tile);
        g_TileList[tile] = SsrPackTile(pixel);
    }
    else if (localIndex == 0u)
    {
        g_Average[group.xy] = float4(0.0, 0.0, 0.0, 0.0);
    }
}
