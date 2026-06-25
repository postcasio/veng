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

    int2 dst = int2(id.xy);

    // Mip 0 ingests the depth target 1:1 (source and dest extents match).
    if (g_PC.SourceExtent.x == g_PC.DestExtent.x && g_PC.SourceExtent.y == g_PC.DestExtent.y)
    {
        g_Dest[dst] = LoadClamped(dst);
        return;
    }

    int2 base = dst * 2;

    float m = LoadClamped(base);
    m = min(m, LoadClamped(base + int2(1, 0)));
    m = min(m, LoadClamped(base + int2(0, 1)));
    m = min(m, LoadClamped(base + int2(1, 1)));

    // An odd parent dimension drops its last row/column from every 2x2 footprint; fold it
    // in with min so no near sample is lost (a dropped near sample would make the pyramid
    // non-conservative — it would report a region emptier than it is and skip a real hit).
    bool oddX = (g_PC.SourceExtent.x & 1u) != 0u;
    bool oddY = (g_PC.SourceExtent.y & 1u) != 0u;

    if (oddX)
    {
        int x = base.x + 2;
        m = min(m, LoadClamped(int2(x, base.y)));
        m = min(m, LoadClamped(int2(x, base.y + 1)));
    }
    if (oddY)
    {
        int y = base.y + 2;
        m = min(m, LoadClamped(int2(base.x, y)));
        m = min(m, LoadClamped(int2(base.x + 1, y)));
    }
    if (oddX && oddY)
    {
        m = min(m, LoadClamped(int2(base.x + 2, base.y + 2)));
    }

    g_Dest[dst] = m;
}
