// Curl of the advected velocity, the scalar field vorticity confinement steers by. The x
// derivative carries the per-row metric; the y derivative does not, since the metric stretches
// the x axis alone.
#include "fluid_common.slang"
#include "../Veng/binding.slang"

struct FluidCurlSet3
{
    Texture2D<float4> Velocity;
    [format("r32f")] RWTexture2D<float> Curl;
    StructuredBuffer<float> RowMetric;
};
VE_DESCRIPTOR_SET(3, FluidCurlSet3, g_Set3);

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (FluidOutside(id))
        return;

    int2 texel = int2(id.xy);
    float metric = FluidRowMetric(g_Set3.RowMetric, texel.y);

    float right = FluidLoad(g_Set3.Velocity, texel + int2(1, 0)).y;
    float left = FluidLoad(g_Set3.Velocity, texel + int2(-1, 0)).y;
    float up = FluidLoad(g_Set3.Velocity, texel + int2(0, 1)).x;
    float down = FluidLoad(g_Set3.Velocity, texel + int2(0, -1)).x;

    g_Set3.Curl[texel] = metric * (right - left) * 0.5 - (up - down) * 0.5;
}
