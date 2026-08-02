// Moves an advected field out of the rgba32f scratch and into the caller's own image, scaling
// by the field's per-step survival factor (1 - dissipation * dt) on the way. Advection cannot
// run in place — it reads arbitrary texels — so this is the second half of every dye's step.
//
// A variant defines FLUID_STORE_FORMAT, FLUID_STORE_TYPE and FLUID_STORE_SWIZZLE before
// including this; the destination format must match the caller's image exactly.
#include "fluid_common.slang"

[[vk::binding(0, 1)]] Texture2D<float4> g_Source;
[[vk::binding(1, 1)]] [format(FLUID_STORE_FORMAT)] RWTexture2D<FLUID_STORE_TYPE> g_Dest;

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (FluidOutside(id))
        return;

    int2 texel = int2(id.xy);
    float4 value = g_Source.Load(int3(texel, 0)) * g_Fluid.Scale;
    g_Dest[texel] = value.FLUID_STORE_SWIZZLE;
}
