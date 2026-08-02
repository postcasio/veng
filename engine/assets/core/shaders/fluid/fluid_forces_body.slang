// The force stage, shared by the destination-format variants that include it: vorticity
// confinement from the curl field, relaxation toward an optional target field, and an optional
// damping mask, applied to the advected velocity and written into the caller's velocity image.
//
// A variant defines FLUID_FORCES_FORMAT (the storage-image format string) and
// FLUID_FORCES_TYPE (the matching texel type) before including this. The destination format
// must match the caller's velocity image exactly, which is why the stage is a family rather
// than one kernel.
#include "fluid_common.slang"

[[vk::binding(0, 1)]] Texture2D<float4> g_Curl;
[[vk::binding(1, 1)]] Texture2D<float4> g_Source;
[[vk::binding(2, 1)]] Texture2D<float4> g_Target;
[[vk::binding(3, 1)]] Texture2D<float4> g_Damping;
[[vk::binding(4, 1)]] [format(FLUID_FORCES_FORMAT)] RWTexture2D<FLUID_FORCES_TYPE> g_Dest;
[[vk::binding(5, 1)]] StructuredBuffer<float> g_RowMetric;

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (FluidOutside(id))
        return;

    int2 texel = int2(id.xy);
    float2 velocity = g_Source.Load(int3(texel, 0)).xy;
    float metric = FluidRowMetric(g_RowMetric, texel.y);

    // Confinement pushes velocity along the curl gradient rotated a quarter turn, feeding energy
    // back into the eddies the advection's bilinear taps diffuse away.
    if (g_Fluid.Scale != 0.0)
    {
        float right = abs(FluidLoad(g_Curl, texel + int2(1, 0)).x);
        float left = abs(FluidLoad(g_Curl, texel + int2(-1, 0)).x);
        float up = abs(FluidLoad(g_Curl, texel + int2(0, 1)).x);
        float down = abs(FluidLoad(g_Curl, texel + int2(0, -1)).x);

        float2 gradient = float2(metric * (right - left) * 0.5, (up - down) * 0.5);
        float lengthSq = dot(gradient, gradient);
        if (lengthSq > 1e-12)
        {
            float2 normal = gradient * rsqrt(lengthSq);
            float curl = g_Curl.Load(int3(texel, 0)).x;
            velocity +=
                g_Fluid.TimeStep * g_Fluid.Scale * float2(normal.y * curl, -normal.x * curl);
        }
    }

    if ((g_Fluid.Flags & FluidFlagHasTarget) != 0u)
    {
        float2 target = g_Target.Load(int3(texel, 0)).xy;
        velocity += g_Fluid.TimeStep * g_Fluid.Relaxation * (target - velocity);
    }

    if ((g_Fluid.Flags & FluidFlagHasDamping) != 0u)
        velocity *= g_Damping.Load(int3(texel, 0)).x;

    g_Dest[texel] = FLUID_FORCES_TYPE(velocity);
}
