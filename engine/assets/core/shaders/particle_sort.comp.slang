// Particle Sort: a bitonic sort of the alpha keys, ascending, over a power-of-two length P of at
// least 1024. A key is (the bitwise complement of the view distance's float bits, draw index), so
// ascending order is back to front. Three entries:
//
// - csSortLocal sorts blocks of 1024 keys entirely in groupshared memory, reading an index at or
//   past the frame's alpha count as the sentinel key 0xFFFFFFFF, so the sentinels sort to the end
//   and no padding pass is needed;
// - csSortGlobal is one compare-exchange step (stage k, distance j >= 1024) over device memory;
// - csSortMergeLocal finishes the steps j < 1024 of stage k in groupshared memory.
#include "particle_compute.slang"

static const uint SortBlock = 1024u;
static const uint SortThreads = 512u;
static const uint SortGlobalThreads = 256u;

groupshared uint2 s_Keys[SortBlock];

// Orders the pair (a, b) ascending or descending by key.
void OrderPair(inout uint2 a, inout uint2 b, bool ascending)
{
    if ((a.x > b.x) == ascending)
    {
        uint2 t = a;
        a = b;
        b = t;
    }
}

// One compare-exchange of the step (k, j) in groupshared memory, for local thread t of a block
// starting at base.
void ExchangeShared(uint t, uint k, uint j, uint base)
{
    uint i = 2u * t - (t & (j - 1u));
    uint l = i + j;
    uint2 a = s_Keys[i];
    uint2 b = s_Keys[l];
    OrderPair(a, b, ((base + i) & k) == 0u);
    s_Keys[i] = a;
    s_Keys[l] = b;
}

[shader("compute")]
[numthreads(512, 1, 1)]
void csSortLocal(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    uint base = group.x * SortBlock;
    uint count = g_Set3.Counters.Load(ParticleCounterAlpha);
    for (uint i = thread.x; i < SortBlock; i += SortThreads)
    {
        uint index = base + i;
        s_Keys[i] = index < count ? g_Set3.AlphaKeys[index] : uint2(0xFFFFFFFFu, 0xFFFFFFFFu);
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint k = 2u; k <= SortBlock; k <<= 1u)
    {
        for (uint j = k >> 1u; j > 0u; j >>= 1u)
        {
            ExchangeShared(thread.x, k, j, base);
            GroupMemoryBarrierWithGroupSync();
        }
    }
    for (uint i = thread.x; i < SortBlock; i += SortThreads)
    {
        g_Set3.AlphaKeys[base + i] = s_Keys[i];
    }
}

[shader("compute")]
[numthreads(256, 1, 1)]
void csSortGlobal(uint3 id : SV_DispatchThreadID)
{
    uint t = id.x;
    if (t >= g_PC.SortSize / 2u)
    {
        return;
    }
    uint j = g_PC.SortStep;
    uint i = 2u * t - (t & (j - 1u));
    uint l = i + j;
    uint2 a = g_Set3.AlphaKeys[i];
    uint2 b = g_Set3.AlphaKeys[l];
    OrderPair(a, b, (i & g_PC.SortStage) == 0u);
    g_Set3.AlphaKeys[i] = a;
    g_Set3.AlphaKeys[l] = b;
}

[shader("compute")]
[numthreads(512, 1, 1)]
void csSortMergeLocal(uint3 group : SV_GroupID, uint3 thread : SV_GroupThreadID)
{
    uint base = group.x * SortBlock;
    for (uint i = thread.x; i < SortBlock; i += SortThreads)
    {
        s_Keys[i] = g_Set3.AlphaKeys[base + i];
    }
    GroupMemoryBarrierWithGroupSync();
    for (uint j = SortBlock >> 1u; j > 0u; j >>= 1u)
    {
        ExchangeShared(thread.x, g_PC.SortStage, j, base);
        GroupMemoryBarrierWithGroupSync();
    }
    for (uint i = thread.x; i < SortBlock; i += SortThreads)
    {
        g_Set3.AlphaKeys[base + i] = s_Keys[i];
    }
}
