// Semi-Lagrangian advection: a texel back-traces its centre one timestep along the velocity
// field and reads the source there. Unconditionally stable, so the timestep is a quality knob
// rather than a stability constraint.
//
// The destination is the solver's rgba32f scratch, so this one kernel serves the velocity field
// and every dye whatever format each of those carries; a per-format store kernel moves the
// result back out.
#include "fluid_common.slang"

[[vk::binding(0, 3)]] Texture2D<float4> g_Source;
[[vk::binding(1, 3)]] Texture2D<float4> g_Velocity;
[[vk::binding(2, 3)]] [format("rgba32f")] RWTexture2D<float4> g_Dest;
[[vk::binding(3, 3)]] StructuredBuffer<float> g_RowMetric;

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (FluidOutside(id))
        return;

    int2 texel = int2(id.xy);
    float2 velocity = g_Velocity.Load(int3(texel, 0)).xy;
    float metric = FluidRowMetric(g_RowMetric, texel.y);

    float2 centre = float2(id.xy) + 0.5;
    float2 source = centre - g_Fluid.TimeStep * float2(velocity.x * metric, velocity.y);

    g_Dest[texel] = FluidSampleBilinear(g_Source, source);
}
