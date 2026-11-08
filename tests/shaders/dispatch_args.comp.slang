// Writes two dispatch-indirect records, (x, 1, 1) each, the way a classifier writes the group
// counts of the passes that consume its lists. Set 0 is reserved for the bindless registry, so the
// buffer binds on set 3.

#include "Veng/binding.slang"
struct DispatchArgsSet3
{
    RWStructuredBuffer<uint> Arguments;
};
VE_DESCRIPTOR_SET(3, DispatchArgsSet3, g_Set3);

struct DispatchArgsPush
{
    uint FirstGroups;
    uint SecondGroups;
};
VE_PUSH_CONSTANTS(DispatchArgsPush, u_Push);

[shader("compute")]
[numthreads(1, 1, 1)]
void csMain()
{
    g_Set3.Arguments[0] = u_Push.FirstGroups;
    g_Set3.Arguments[1] = 1u;
    g_Set3.Arguments[2] = 1u;
    g_Set3.Arguments[3] = u_Push.SecondGroups;
    g_Set3.Arguments[4] = 1u;
    g_Set3.Arguments[5] = 1u;
}
