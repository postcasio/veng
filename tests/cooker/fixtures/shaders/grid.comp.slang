// A compute kernel with a distinct size on every axis, so the cooked threadgroup size is checked
// axis by axis.

[[vk::binding(0, 3)]] RWStructuredBuffer<uint> g_Output;

[shader("compute")]
[numthreads(8, 4, 2)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    g_Output[id.x + id.y * 8 + id.z * 32] = id.x;
}
