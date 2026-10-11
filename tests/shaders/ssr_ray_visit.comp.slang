// Visits a stochastic reflection chain's ray list through the dispatch record its argument pass
// wrote: each invocation past the list's length returns, and each one inside it adds one to the
// visit count of the pixel its ray names, so a ray visited twice or never shows. Set 0 is reserved
// for the bindless registry, so the buffers bind on set 3.
#include "Veng/ssr_ray_list.slang"

[[vk::binding(0, 3)]] RWStructuredBuffer<uint> u_RayList;
[[vk::binding(1, 3)]] RWStructuredBuffer<uint> u_Counters;
[[vk::binding(2, 3)]] RWStructuredBuffer<uint> u_Visits;

struct RayVisitPush
{
    uint Width;  // the render extent's width, the visit counts' row stride
};
[[vk::push_constant]] RayVisitPush u_Push;

[shader("compute")]
[numthreads(64, 1, 1)]
void csMain(uint3 group : SV_GroupID, uint localIndex : SV_GroupIndex)
{
    uint index = group.x * 64u + localIndex;
    if (index >= u_Counters[SsrRayCountCounter])
        return;
    SsrRay ray = SsrUnpackRay(u_RayList[index]);
    InterlockedAdd(u_Visits[ray.Pixel.y * u_Push.Width + ray.Pixel.x], 1u);
}
