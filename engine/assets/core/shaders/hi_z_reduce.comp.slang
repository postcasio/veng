// Hi-Z max-Z mip reduction: one destination mip per dispatch. Each invocation owns
// one destination texel and folds the max of its source-mip footprint into it.
//
// The source is read through a plain Texture2D (point Load, no sampler) so mip 0
// can read the D32Sfloat depth target and mip n>0 can read the prior R32Sfloat hi-Z
// mip through the same pipeline. The destination is a single R32Sfloat storage mip.
//
// Max-Z is the conservative reduction for the engine's [0,1] depth (near = 0, far =
// 1): a coarse texel stores the farthest depth over its footprint, so an occlusion
// test against it never reports a footprint occluded when a nearer (smaller-depth)
// sample exists in it.
//
// Set 0 is reserved across every pipeline layout for the bindless registry; this
// closed producer→consumer reduction binds its own resources on set 1.
[[vk::binding(0, 1)]] Texture2D<float> g_Source;
[[vk::binding(1, 1)]] [format("r32f")] RWTexture2D<float> g_Dest;

struct PushConstants
{
    uint2 DestExtent;    // destination mip dimensions in texels
    uint2 SourceExtent;  // source mip dimensions in texels
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

// Point-load a source texel, clamping the coordinate into the source extent. The
// edge clamp keeps every read a real depth value (never a spurious 0), which the
// CPU reference reduction mirrors — so an out-of-range footprint texel folds in the
// nearest valid sample rather than a fabricated near value.
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

    // Mip 0 ingests the depth target at full resolution (source and dest extents
    // match): a 1:1 copy, the base of the chain. Mip n>0 halves its parent, so each
    // destination texel maxes a 2x2 (or 3x3 on an odd parent) footprint.
    if (g_PC.SourceExtent.x == g_PC.DestExtent.x && g_PC.SourceExtent.y == g_PC.DestExtent.y)
    {
        g_Dest[dst] = LoadClamped(dst);
        return;
    }

    int2 base = dst * 2;

    // The 2x2 parent footprint of this destination texel.
    float m = LoadClamped(base);
    m = max(m, LoadClamped(base + int2(1, 0)));
    m = max(m, LoadClamped(base + int2(0, 1)));
    m = max(m, LoadClamped(base + int2(1, 1)));

    // When a parent dimension is odd, halving drops its last row/column from every
    // 2x2 footprint. Fold the extra row/column in so no far sample is lost — a
    // dropped far sample would make the pyramid less conservative.
    bool oddX = (g_PC.SourceExtent.x & 1u) != 0u;
    bool oddY = (g_PC.SourceExtent.y & 1u) != 0u;

    if (oddX)
    {
        int x = base.x + 2;
        m = max(m, LoadClamped(int2(x, base.y)));
        m = max(m, LoadClamped(int2(x, base.y + 1)));
    }
    if (oddY)
    {
        int y = base.y + 2;
        m = max(m, LoadClamped(int2(base.x, y)));
        m = max(m, LoadClamped(int2(base.x + 1, y)));
    }
    if (oddX && oddY)
    {
        m = max(m, LoadClamped(int2(base.x + 2, base.y + 2)));
    }

    g_Dest[dst] = m;
}
