// Records what each subgroup operation returns to each invocation of one 64-wide workgroup, so a
// test can check every answer against the operation's definition on whatever subgroup layout the
// device chose: which invocations share a subgroup, and in which lane, is read back rather than
// assumed. Set 0 is reserved for the bindless registry, so the buffers bind on set 3.
[[vk::binding(0, 3)]] RWStructuredBuffer<uint4> u_Lanes;   // (local index, lane, lane count, first lane's local index)
[[vk::binding(1, 3)]] RWStructuredBuffer<uint4> u_Results; // (active count, prefix count, quad across X, predicate)

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
    u_Lanes[localIndex] =
        uint4(localIndex, WaveGetLaneIndex(), WaveGetLaneCount(), WaveReadLaneFirst(localIndex));
    u_Results[localIndex] = uint4(WaveActiveCountBits(predicate), WavePrefixCountBits(predicate),
                                  QuadReadAcrossX(localIndex), predicate ? 1u : 0u);
}
