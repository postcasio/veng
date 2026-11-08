// Counts what an indirect dispatch ran: every invocation adds one to the dispatch's invocation
// counter, and each group's first invocation adds one to that group's own slot, so a group run
// twice or never shows. Set 0 is reserved for the bindless registry, so the buffer binds on set 3.

#include "Veng/binding.slang"
struct DispatchVisitSet3
{
    RWStructuredBuffer<uint> Visits;
};
VE_DESCRIPTOR_SET(3, DispatchVisitSet3, g_Set3);

struct DispatchVisitPush
{
    // The dispatch's invocation counter; its group slots follow it.
    uint Base;
};
VE_PUSH_CONSTANTS(DispatchVisitPush, u_Push);

[shader("compute")]
[numthreads(64, 1, 1)]
void csMain(uint3 group : SV_GroupID, uint localIndex : SV_GroupIndex)
{
    InterlockedAdd(g_Set3.Visits[u_Push.Base], 1u);
    if (localIndex == 0u)
    {
        InterlockedAdd(g_Set3.Visits[u_Push.Base + 1u + group.x], 1u);
    }
}
