// Divergence of the post-force velocity — the right-hand side of the pressure Poisson solve.
#include "fluid_common.slang"

[[vk::binding(0, 1)]] Texture2D<float4> g_Velocity;
[[vk::binding(1, 1)]] [format("r32f")] RWTexture2D<float> g_Divergence;
[[vk::binding(2, 1)]] StructuredBuffer<float> g_RowMetric;

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (FluidOutside(id))
        return;

    int2 texel = int2(id.xy);
    float metric = FluidRowMetric(g_RowMetric, texel.y);

    float right = FluidLoad(g_Velocity, texel + int2(1, 0)).x;
    float left = FluidLoad(g_Velocity, texel + int2(-1, 0)).x;
    float up = FluidLoad(g_Velocity, texel + int2(0, 1)).y;
    float down = FluidLoad(g_Velocity, texel + int2(0, -1)).y;

    g_Divergence[texel] = metric * (right - left) * 0.5 + (up - down) * 0.5;
}
