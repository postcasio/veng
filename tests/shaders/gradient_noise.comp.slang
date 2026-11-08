// Evaluates host/gradient_noise.slang's fractal sum on the GPU at a list of points, so a test can
// compare it with the same source compiled for the host. Sets 0-2 are reserved for the bindless
// registries, so the buffers bind on set 3, matching the test's hand-built layout.
#include "host/gradient_noise.slang"
#include "Veng/binding.slang"

struct GradientNoiseSet3
{
    ByteAddressBuffer Points;  // float4: xyz point, w octaves (bits)
    RWStructuredBuffer<float4> Results;  // value in .x, gradient in .yzw
};
VE_DESCRIPTOR_SET(3, GradientNoiseSet3, g_Set3);

struct GradientNoisePush
{
    uint PointCount;
};
VE_PUSH_CONSTANTS(GradientNoisePush, u_Push);

[shader("compute")]
[numthreads(64, 1, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= u_Push.PointCount)
    {
        return;
    }

    const float4 point = g_Set3.Points.Load<float4>(id.x * 16);
    g_Set3.Results[id.x] = GradientNoiseFbm<float>(point.xyz, asuint(point.w));
}
