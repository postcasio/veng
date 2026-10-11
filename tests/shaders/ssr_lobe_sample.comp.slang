// Draws reflection directions from the stochastic reflection chain's GGX lobe sampler on a
// stratified grid of 2D samples: invocation i takes the cell (i % Side, i / Side) of a Side x Side
// grid at its centre, and writes the direction a ray along ViewRay leaving a surface of Normal
// reflects in. Set 0 is reserved for the bindless registry, so the buffer binds on set 3.
#include "Veng/ssr_sample.slang"

[[vk::binding(0, 3)]] RWStructuredBuffer<float4> u_Directions;

struct LobeSamplePush
{
    float4 ViewRay;  // xyz: unit, from the eye toward the surface
    float4 Normal;   // xyz: the surface's unit normal
    float Alpha;     // GGX alpha
    uint Side;       // the stratified grid's edge
};
[[vk::push_constant]] LobeSamplePush u_Push;

[shader("compute")]
[numthreads(64, 1, 1)]
void csMain(uint3 thread : SV_DispatchThreadID)
{
    uint index = thread.x;
    if (index >= u_Push.Side * u_Push.Side)
        return;
    float2 u = (float2(index % u_Push.Side, index / u_Push.Side) + 0.5) / float(u_Push.Side);
    float3 direction = SampleReflectionVector(u_Push.ViewRay.xyz, u_Push.Normal.xyz, u_Push.Alpha, u);
    u_Directions[index] = float4(direction, 0.0);
}
