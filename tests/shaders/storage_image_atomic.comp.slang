// Adds one into a small R32Uint storage image from every invocation of a dispatch much larger than
// it, so each texel is the target of many concurrent atomics from many workgroups. Mode 0 instead
// zeroes the image with plain stores, which the test runs as a pass ahead of the adds.

#include "Veng/binding.slang"
struct StorageImageAtomicSet3
{
    [format("r32ui")] RWTexture2D<uint> Target;
};
VE_DESCRIPTOR_SET(3, StorageImageAtomicSet3, g_Set3);

struct AtomicPush
{
    uint2 Extent;
    uint Mode;
    uint Pad;
};
VE_PUSH_CONSTANTS(AtomicPush, u_Push);

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (u_Push.Mode == 0u)
    {
        if (all(id.xy < u_Push.Extent))
        {
            g_Set3.Target[id.xy] = 0u;
        }
        return;
    }
    InterlockedAdd(g_Set3.Target[id.xy % u_Push.Extent], 1u);
}
