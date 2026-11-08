// Semi-Lagrangian advection: a texel back-traces its centre one timestep along the velocity
// field and reads the source there. Unconditionally stable, so the timestep is a quality knob
// rather than a stability constraint.
//
// The destination is the solver's rgba32f scratch, so this one kernel serves the velocity field
// and every dye whatever format each of those carries; a per-format store kernel moves the
// result back out.
#include "fluid_common.slang"
#include "../Veng/binding.slang"

struct FluidAdvectSet3
{
    Texture2D<float4> Source;
    Texture2D<float4> Velocity;
    [format("rgba32f")] RWTexture2D<float4> Dest;
    StructuredBuffer<float> RowMetric;
};
VE_DESCRIPTOR_SET(3, FluidAdvectSet3, g_Set3);

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (FluidOutside(id))
        return;

    int2 texel = int2(id.xy);
    float2 velocity = g_Set3.Velocity.Load(int3(texel, 0)).xy;
    float metric = FluidRowMetric(g_Set3.RowMetric, texel.y);

    float2 centre = float2(id.xy) + 0.5;
    float2 source = centre - g_Fluid.TimeStep * float2(velocity.x * metric, velocity.y);

    g_Set3.Dest[texel] = (g_Fluid.Flags & FluidFlagCubicAdvect) != 0u
                        ? FluidSampleCatmullRom(g_Set3.Source, source)
                        : FluidSampleBilinear(g_Set3.Source, source);
}
