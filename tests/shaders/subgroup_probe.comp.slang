// Records what each subgroup operation returns to each invocation of one 64-wide workgroup, so a
// test can check every answer against the operation's definition on whatever subgroup layout the
// device chose: which invocations share a subgroup, and in which lane, is read back rather than
// assumed. Set 0 is reserved for the bindless registry, so the buffers bind on set 3.

#include "Veng/binding.slang"
struct SubgroupProbeSet3
{
    RWStructuredBuffer<uint4> Lanes;  // (local index, lane, lane count, first lane's local index)
    RWStructuredBuffer<uint4> Results;  // (active count, prefix count, quad across X, predicate)
};
VE_DESCRIPTOR_SET(3, SubgroupProbeSet3, g_Set3);

// An irregular predicate, so neither count is a function of the lane alone.
bool Predicate(uint localIndex)
{
    return ((localIndex * 7u + 3u) % 5u) < 2u;
}

[shader("compute")]
[numthreads(64, 1, 1)]
void csMain(uint localIndex : SV_GroupIndex)
{
    const bool predicate = Predicate(localIndex);
    g_Set3.Lanes[localIndex] =
        uint4(localIndex, WaveGetLaneIndex(), WaveGetLaneCount(), WaveReadLaneFirst(localIndex));
    g_Set3.Results[localIndex] = uint4(WaveActiveCountBits(predicate), WavePrefixCount(predicate),
                                  QuadReadAcrossX(localIndex), predicate ? 1u : 0u);
}
