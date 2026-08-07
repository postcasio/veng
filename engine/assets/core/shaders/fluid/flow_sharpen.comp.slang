// Clamped unsharp mask: a texel is pushed away from its 3x3 neighbourhood mean by the strength,
// then clamped to that neighbourhood's own per-channel range. The clamp is what makes it safe in a
// feedback loop — an unclamped unsharp amplifies each pass's overshoot and diverges — so the result
// is never brighter than the brightest neighbour nor darker than the darkest.
//
// The destination is the flow field's rgba32f scratch, so this one kernel serves a dye of any
// format; a per-format store kernel moves the result back out. Shares fluid_common.slang's wrap
// fold and push block with the advection it companions.
#include "fluid_common.slang"

[[vk::binding(0, 1)]] Texture2D<float4> g_Source;
[[vk::binding(1, 1)]] [format("rgba32f")] RWTexture2D<float4> g_Dest;

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (FluidOutside(id))
        return;

    int2 texel = int2(id.xy);
    float4 centre = FluidLoad(g_Source, texel);

    float4 sum = float4(0.0);
    float4 lo = centre;
    float4 hi = centre;
    for (int dy = -1; dy <= 1; ++dy)
    {
        for (int dx = -1; dx <= 1; ++dx)
        {
            float4 s = FluidLoad(g_Source, texel + int2(dx, dy));
            sum += s;
            lo = min(lo, s);
            hi = max(hi, s);
        }
    }
    float4 blur = sum / 9.0;

    float strength = g_Fluid.Scale;
    float4 sharpened = centre + strength * (centre - blur);
    g_Dest[texel] = clamp(sharpened, lo, hi);
}
