// Evaluates host/gradient_noise.slang's fractal sum on the GPU at a list of points, so a test can
// compare it with the same source compiled for the host. Sets 0-2 are reserved for the bindless
// registries, so the buffers bind on set 3, matching the test's hand-built layout.
#include "host/gradient_noise.slang"

[[vk::binding(0, 3)]] ByteAddressBuffer u_Points;            // float4: xyz point, w octaves (bits)
[[vk::binding(1, 3)]] RWStructuredBuffer<float4> u_Results; // value in .x, gradient in .yzw

struct GradientNoisePush
{
    uint PointCount;
};
[[vk::push_constant]] GradientNoisePush u_Push;

[shader("compute")]
[numthreads(64, 1, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= u_Push.PointCount)
    {
        return;
    }

    const float4 point = u_Points.Load<float4>(id.x * 16);
    u_Results[id.x] = GradientNoiseFbm<float>(point.xyz, asuint(point.w));
}
