// Gradient subtraction, the second half of the projection: the pressure gradient comes off the
// velocity in place, leaving it divergence-free. A clamped axis is a wall, so the wall-normal
// component is zeroed at the outermost texels — free slip, and the reason those texels carry
// residual divergence the interior does not.
//
// A variant defines FLUID_GRADIENT_FORMAT and FLUID_GRADIENT_TYPE before including this; the
// destination format must match the caller's velocity image exactly.
#include "fluid_common.slang"

[[vk::binding(0, 1)]] Texture2D<float4> g_Pressure;
[[vk::binding(1, 1)]] [format(FLUID_GRADIENT_FORMAT)] RWTexture2D<FLUID_GRADIENT_TYPE> g_Velocity;
[[vk::binding(2, 1)]] StructuredBuffer<float> g_RowMetric;

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (FluidOutside(id))
        return;

    int2 texel = int2(id.xy);
    float metric = FluidRowMetric(g_RowMetric, texel.y);

    float right = FluidLoad(g_Pressure, texel + int2(1, 0)).x;
    float left = FluidLoad(g_Pressure, texel + int2(-1, 0)).x;
    float up = FluidLoad(g_Pressure, texel + int2(0, 1)).x;
    float down = FluidLoad(g_Pressure, texel + int2(0, -1)).x;

    float2 velocity = float2(g_Velocity[texel]);
    velocity -= float2(metric * (right - left) * 0.5, (up - down) * 0.5);

    if (g_Fluid.WrapX == FluidWrapClamped && (texel.x == 0 || texel.x == int(g_Fluid.Width) - 1))
        velocity.x = 0.0;
    if (g_Fluid.WrapY == FluidWrapClamped && (texel.y == 0 || texel.y == int(g_Fluid.Height) - 1))
        velocity.y = 0.0;

    g_Velocity[texel] = FLUID_GRADIENT_TYPE(velocity);
}
