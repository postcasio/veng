// One Jacobi relaxation of the pressure Poisson equation, ping-ponged between two pressure
// images. The metric scales the x half of the Laplacian by m^2, so the stencil weight is
// 2m^2 + 2 rather than 4. A clamped axis folds by clamping, which mirrors the neighbour and is
// the Neumann condition a wall wants.
#include "fluid_common.slang"

[[vk::binding(0, 1)]] Texture2D<float4> g_Pressure;
[[vk::binding(1, 1)]] Texture2D<float4> g_Divergence;
[[vk::binding(2, 1)]] [format("r32f")] RWTexture2D<float> g_Dest;
[[vk::binding(3, 1)]] StructuredBuffer<float> g_RowMetric;

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (FluidOutside(id))
        return;

    int2 texel = int2(id.xy);
    float metric = FluidRowMetric(g_RowMetric, texel.y);
    float metricSq = metric * metric;

    float neighbours = 0.0;
    if ((g_Fluid.Flags & FluidFlagFirstIteration) == 0u)
    {
        float right = FluidLoad(g_Pressure, texel + int2(1, 0)).x;
        float left = FluidLoad(g_Pressure, texel + int2(-1, 0)).x;
        float up = FluidLoad(g_Pressure, texel + int2(0, 1)).x;
        float down = FluidLoad(g_Pressure, texel + int2(0, -1)).x;
        neighbours = metricSq * (right + left) + (up + down);
    }

    float divergence = g_Divergence.Load(int3(texel, 0)).x;
    g_Dest[texel] = (neighbours - divergence) / (2.0 * metricSq + 2.0);
}
