// A compute kernel with a distinct size on every axis, so the cooked threadgroup size is checked
// axis by axis.

#include "Veng/binding.slang"
struct FixtureSet3
{
    RWStructuredBuffer<uint> Output;
};
VE_DESCRIPTOR_SET(3, FixtureSet3, g_Set3);
#define g_Output g_Set3.Output

[shader("compute")]
[numthreads(8, 4, 2)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    g_Output[id.x + id.y * 8 + id.z * 32] = id.x;
}
