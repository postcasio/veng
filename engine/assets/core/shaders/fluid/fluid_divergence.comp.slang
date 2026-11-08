// Divergence of the post-force velocity — the right-hand side of the pressure Poisson solve.
#include "fluid_common.slang"
#include "../Veng/binding.slang"

struct FluidDivergenceSet3
{
    Texture2D<float4> Velocity;
    [format("r32f")] RWTexture2D<float> Divergence;
    StructuredBuffer<float> RowMetric;
};
VE_DESCRIPTOR_SET(3, FluidDivergenceSet3, g_Set3);

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (FluidOutside(id))
        return;

    int2 texel = int2(id.xy);
    float metric = FluidRowMetric(g_Set3.RowMetric, texel.y);

    float right = FluidLoad(g_Set3.Velocity, texel + int2(1, 0)).x;
    float left = FluidLoad(g_Set3.Velocity, texel + int2(-1, 0)).x;
    float up = FluidLoad(g_Set3.Velocity, texel + int2(0, 1)).y;
    float down = FluidLoad(g_Set3.Velocity, texel + int2(0, -1)).y;

    g_Set3.Divergence[texel] = metric * (right - left) * 0.5 + (up - down) * 0.5;
}
