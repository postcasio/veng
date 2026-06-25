// Conservative hi-Z occlusion test, shared by the isolation compute pass and the
// GPU cull. A candidate's world AABB is projected to a screen-space footprint and
// tested against the previous frame's max-Z hi-Z pyramid: a candidate fully behind
// the farthest stored depth over its whole footprint is occluded.
//
// Conservative is the correctness bar: every uncertainty (a near-plane crossing, an
// off-screen footprint, a straddling depth, a tie) resolves to NOT occluded, so the
// test may leave a hidden draw in but never drops a visible one. The pyramid stores
// max (farthest) depth per texel (see hi_z_reduce.comp), the reduction that makes
// "fully behind the nearest stored depth" sound for the engine's [0,1] depth.

// floor(log2(x)) for a positive integer, by bit scan. ceil(log2(x)) is FloorLog2 of
// (x rounded up to the next power of two): a power of two keeps its own log, anything
// above the previous power rounds up one level.
uint CeilLog2(uint x)
{
    if (x <= 1u)
    {
        return 0u;
    }
    return firstbithigh(x - 1u) + 1u;
}

// Tests whether the world AABB [boundsMin, boundsMax] is provably hidden behind hiZ
// over its screen footprint.
//
// prevViewProj is the previous frame's camera world->clip (the pyramid is last
// frame's depth, so the projection that screen-bounds the candidate must be last
// frame's too). hiZBaseExtent is mip 0's pixel extent. depthBias absorbs reduction
// quantization; a tie draws.
//
// Returns true only when the candidate is occluded; false (draw it) on every
// uncertainty.
bool IsOccluded(float3 boundsMin, float3 boundsMax, float4x4 prevViewProj,
                Texture2D<float> hiZ, uint2 hiZBaseExtent, float depthBias)
{
    // Project the 8 corners. Track the screen-space [0,1] xy bound and the nearest
    // (minimum) projected depth — the candidate's closest point to the camera.
    float2 uvMin = float2(1e30, 1e30);
    float2 uvMax = float2(-1e30, -1e30);
    float zMin = 1e30;

    [unroll]
    for (uint i = 0u; i < 8u; ++i)
    {
        float3 corner = float3((i & 1u) != 0u ? boundsMax.x : boundsMin.x,
                               (i & 2u) != 0u ? boundsMax.y : boundsMin.y,
                               (i & 4u) != 0u ? boundsMax.z : boundsMin.z);

        float4 clip = mul(prevViewProj, float4(corner, 1.0));

        // A corner at or behind the near plane (w <= 0) cannot be screen-bounded
        // safely: the perspective divide flips or explodes. Draw the candidate.
        if (clip.w <= 0.0)
        {
            return false;
        }

        float3 ndc = clip.xyz / clip.w;
        // NDC xy [-1,1] -> UV [0,1]. Vulkan clip z is already [0,1].
        float2 uv = ndc.xy * 0.5 + 0.5;

        uvMin = min(uvMin, uv);
        uvMax = max(uvMax, uv);
        zMin = min(zMin, ndc.z);
    }

    // A footprint reaching outside the screen is the frustum cull's concern, not
    // occlusion's: off-screen-but-shadowing geometry and frustum-edge cases draw.
    if (uvMin.x < 0.0 || uvMin.y < 0.0 || uvMax.x > 1.0 || uvMax.y > 1.0)
    {
        return false;
    }

    // Footprint pixel extent at mip 0.
    float2 baseExtent = float2(hiZBaseExtent);
    float2 footprintPx = (uvMax - uvMin) * baseExtent;
    float maxFootprintPx = max(footprintPx.x, footprintPx.y);

    // Pick the mip whose texel covers the footprint, so it spans at most a 2x2 texel
    // neighborhood at that level (the standard hi-Z LOD pick). A sub-pixel footprint
    // (maxFootprintPx <= 1) tests mip 0.
    //
    // The pyramid has firstbithigh(maxDim) + 1 mips (mip 0..floor(log2(maxDim))). A
    // footprint wider than the largest power of two <= maxDim would pick one mip past
    // the last (e.g. a 1281..1280-wide footprint picks mip 11 on a 1280-wide pyramid
    // whose last mip is 10); that Load reads out of range and returns garbage, which a
    // large object (the ground plane) hits and gets falsely culled by. Clamp to the
    // last real mip — its single coarse texel is the conservative global max-Z.
    uint maxLevel = firstbithigh(max(hiZBaseExtent.x, hiZBaseExtent.y));
    uint level = min(CeilLog2(uint(ceil(max(maxFootprintPx, 1.0)))), maxLevel);

    // The mip's extent and the footprint's texel coordinates at that level.
    uint2 levelExtent = uint2(max(hiZBaseExtent.x >> level, 1u),
                              max(hiZBaseExtent.y >> level, 1u));
    float2 levelExtentF = float2(levelExtent);

    int2 texMin = int2(floor(uvMin * levelExtentF));
    int2 texMax = int2(floor(uvMax * levelExtentF));
    int2 maxCoord = int2(levelExtent) - 1;
    texMin = clamp(texMin, int2(0, 0), maxCoord);
    texMax = clamp(texMax, int2(0, 0), maxCoord);

    // Max of the covered texels (up to a 2x2 neighborhood) is the farthest occluder
    // depth over the footprint. The footprint spans at most two texels per axis at
    // the chosen level, so the inner bound is small and statically unrollable.
    float dFar = 0.0;
    [unroll]
    for (int ty = 0; ty <= 1; ++ty)
    {
        [unroll]
        for (int tx = 0; tx <= 1; ++tx)
        {
            int2 coord = int2(min(texMin.x + tx, texMax.x), min(texMin.y + ty, texMax.y));
            dFar = max(dFar, hiZ.Load(int3(coord, int(level))));
        }
    }

    // Occluded iff the candidate's nearest point is strictly behind the farthest
    // stored depth over the whole footprint. The bias keeps a tie drawing.
    return zMin > dFar + depthBias;
}
