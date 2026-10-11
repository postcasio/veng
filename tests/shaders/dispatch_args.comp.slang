// Writes two dispatch-indirect records, (x, 1, 1) each, the way a classifier writes the group
// counts of the passes that consume its lists. Set 0 is reserved for the bindless registry, so the
// buffer binds on set 3.
[[vk::binding(0, 3)]] RWStructuredBuffer<uint> u_Arguments;

struct DispatchArgsPush
{
    uint FirstGroups;
    uint SecondGroups;
};
[[vk::push_constant]] DispatchArgsPush u_Push;

[shader("compute")]
[numthreads(1, 1, 1)]
void csMain()
{
    u_Arguments[0] = u_Push.FirstGroups;
    u_Arguments[1] = 1u;
    u_Arguments[2] = 1u;
    u_Arguments[3] = u_Push.SecondGroups;
    u_Arguments[4] = 1u;
    u_Arguments[5] = 1u;
}
