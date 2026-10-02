// Hi-Z coarse tail: the pyramid's smallest levels reduced by one workgroup in one dispatch. Each of
// those levels is a handful of texels, so as separate dispatches they are almost entirely
// per-dispatch overhead; here the workgroup reduces them level by level in shared memory and
// synchronizes between levels instead.
//
// The tail is levels First .. First+LevelCount-1, the last being the pyramid's 1x1. Level First is
// reduced from its source — the depth target when First is 0, hi-Z mip First-1 otherwise, which an
// earlier dispatch wrote — and every deeper level from the shared copy of the level above it. Each
// level is exactly hi_z_reduce's: mip 0 a 1:1 copy, mip n>0 the min of its 2x2 parent footprint,
// widened to the extra row or column when a parent dimension is odd, every read clamped into the
// parent's extent. Every tail level is written to its storage mip.
#define TAIL_CAPACITY 7168
#define THREADS 256

[[vk::binding(0, 3)]] Texture2D<float> g_Source;

// One storage binding per tail slot (bindings 1..8), the slots past LevelCount bound but unwritten.
[[vk::binding(1, 3)]] [format("r32f")] RWTexture2D<float> g_Level0;
[[vk::binding(2, 3)]] [format("r32f")] RWTexture2D<float> g_Level1;
[[vk::binding(3, 3)]] [format("r32f")] RWTexture2D<float> g_Level2;
[[vk::binding(4, 3)]] [format("r32f")] RWTexture2D<float> g_Level3;
[[vk::binding(5, 3)]] [format("r32f")] RWTexture2D<float> g_Level4;
[[vk::binding(6, 3)]] [format("r32f")] RWTexture2D<float> g_Level5;
[[vk::binding(7, 3)]] [format("r32f")] RWTexture2D<float> g_Level6;
[[vk::binding(8, 3)]] [format("r32f")] RWTexture2D<float> g_Level7;

struct PushConstants
{
    uint2 Extent;     // the pyramid's level-0 extent
    uint  FirstLevel; // the tail's first level
    uint  LevelCount; // levels in the tail (>= 2)
};

[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

groupshared float g_Texels[TAIL_CAPACITY];

void StoreLevel(uint slot, uint2 id, float value)
{
    switch (slot)
    {
    case 0: g_Level0[id] = value; break;
    case 1: g_Level1[id] = value; break;
    case 2: g_Level2[id] = value; break;
    case 3: g_Level3[id] = value; break;
    case 4: g_Level4[id] = value; break;
    case 5: g_Level5[id] = value; break;
    case 6: g_Level6[id] = value; break;
    default: g_Level7[id] = value; break;
    }
}

uint2 ExtentAt(uint mip)
{
    return max(g_PC.Extent >> mip, uint2(1, 1));
}

uint OffsetOf(uint index)
{
    uint offset = 0;
    for (uint i = 0; i < index; i++)
    {
        uint2 e = ExtentAt(g_PC.FirstLevel + i);
        offset += e.x * e.y;
    }
    return offset;
}

// A clamped point read of the level above tail slot `index`: the source image for slot 0, the
// shared copy of slot index-1 otherwise.
float LoadAbove(uint index, int2 coord, uint2 sourceExtent, uint sourceOffset)
{
    coord = clamp(coord, int2(0, 0), int2(sourceExtent) - 1);
    if (index == 0)
    {
        return g_Source.Load(int3(coord, 0));
    }
    return g_Texels[sourceOffset + uint(coord.y) * sourceExtent.x + uint(coord.x)];
}

[shader("compute")]
[numthreads(THREADS, 1, 1)]
void csMain(uint3 tid : SV_GroupThreadID)
{
    for (uint index = 0; index < g_PC.LevelCount; index++)
    {
        uint mip = g_PC.FirstLevel + index;
        uint2 dest = ExtentAt(mip);
        uint2 source = mip == 0 ? g_PC.Extent : ExtentAt(mip - 1);
        uint offset = OffsetOf(index);
        uint sourceOffset = index == 0 ? 0 : OffsetOf(index - 1);
        bool copy = all(source == dest);
        bool oddX = (source.x & 1u) != 0u;
        bool oddY = (source.y & 1u) != 0u;

        uint count = dest.x * dest.y;
        for (uint t = tid.x; t < count; t += THREADS)
        {
            int2 dst = int2(t % dest.x, t / dest.x);
            float m;
            if (copy)
            {
                m = LoadAbove(index, dst, source, sourceOffset);
            }
            else
            {
                int2 base = dst * 2;
                m = LoadAbove(index, base, source, sourceOffset);
                m = min(m, LoadAbove(index, base + int2(1, 0), source, sourceOffset));
                m = min(m, LoadAbove(index, base + int2(0, 1), source, sourceOffset));
                m = min(m, LoadAbove(index, base + int2(1, 1), source, sourceOffset));
                if (oddX)
                {
                    m = min(m, LoadAbove(index, int2(base.x + 2, base.y), source, sourceOffset));
                    m = min(m, LoadAbove(index, int2(base.x + 2, base.y + 1), source, sourceOffset));
                }
                if (oddY)
                {
                    m = min(m, LoadAbove(index, int2(base.x, base.y + 2), source, sourceOffset));
                    m = min(m, LoadAbove(index, int2(base.x + 1, base.y + 2), source, sourceOffset));
                }
                if (oddX && oddY)
                {
                    m = min(m, LoadAbove(index, base + int2(2, 2), source, sourceOffset));
                }
            }
            g_Texels[offset + t] = m;
            StoreLevel(index, uint2(dst), m);
        }
        GroupMemoryBarrierWithGroupSync();
    }
}
