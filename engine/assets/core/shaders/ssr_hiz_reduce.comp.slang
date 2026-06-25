// SSR min-Z depth reduction: one destination mip per dispatch. Each invocation owns one
// destination texel and folds the MIN of its source-mip footprint into it — the closest
// surface over that footprint. This is the opposite reduction from the occlusion-culling
// max-Z pyramid: SSR marches a ray and needs "is the nearest surface in this region at
// depth Z" so it can declare a cell empty in front of the ray and skip it, whereas
// occlusion culling needs the farthest depth.
//
// The source is a plain Texture2D (point Load): mip 0 reads the D32Sfloat depth target,
// mip n>0 the prior R32Sfloat min-Z mip, through the same pipeline. Engine [0,1] depth
// (near = 0, far = 1), so the background's cleared 1.0 is the farthest value and a real
// surface's smaller depth wins the min.
//
// Set 0 is reserved for the bindless registry; this closed producer->consumer reduction
// binds its own resources on set 1 (the hi-Z reduce set layout: sampled source + storage dest).
[[vk::binding(0, 1)]] Texture2D<float> g_Source;
[[vk::binding(1, 1)]] [format("r32f")] RWTexture2D<float> g_Dest;

struct PushConstants
{
    uint2 DestExtent;    // destination mip dimensions in texels
    uint2 SourceExtent;  // source mip dimensions in texels
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

// Point-load a source texel, clamping into the source extent so an out-of-range footprint
// texel folds in the nearest valid sample rather than a fabricated value.
float LoadClamped(int2 coord)
{
    int2 maxCoord = int2(g_PC.SourceExtent) - 1;
    coord = clamp(coord, int2(0, 0), maxCoord);
    return g_Source.Load(int3(coord, 0));
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= g_PC.DestExtent.x || id.y >= g_PC.DestExtent.y)
        return;

    // Each dest texel folds the MIN of its proportional source footprint — the closest surface
    // over that region. The footprint is [start, end) per axis, an exclusive ceil so adjacent
    // dest cells share a boundary texel: overlap only makes the min more conservative (it never
    // reports a region emptier than it is, so it never skips a real hit). This one mapping covers
    // the 1:1 mip-0 copy at full SSR resolution, the 2:1 chain steps (including an odd parent
    // dimension), and the N:1 mip-0 downsample when SSR runs below full resolution (4:1 at quarter).
    uint2 start = (id.xy * g_PC.SourceExtent) / g_PC.DestExtent;
    uint2 end = ((id.xy + 1u) * g_PC.SourceExtent + g_PC.DestExtent - 1u) / g_PC.DestExtent;

    float m = 1.0; // engine far depth (background); a nearer surface wins the min
    for (uint y = start.y; y < end.y; ++y)
    {
        for (uint x = start.x; x < end.x; ++x)
        {
            m = min(m, LoadClamped(int2(int(x), int(y))));
        }
    }

    g_Dest[int2(id.xy)] = m;
}
