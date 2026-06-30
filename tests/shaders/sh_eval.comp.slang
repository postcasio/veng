// Evaluates the core GPU spherical-harmonics irradiance reconstruction (Veng/sh.slang's
// ShEvalIrradiance) for a set of normals against one coefficient set, so a test can compare
// the GPU result to the CPU EvalIrradiance and the checked-in golden constants. The whole
// point is sharing the exact basis include with the deferred lighting pass — this shader
// pulls in the same Veng/sh.slang the engine does, so any basis/normalization drift between
// CPU and GPU fails the consistency test. Set 0 is reserved for the bindless registry, so the
// buffers bind on set 1, matching the test's hand-built layout.
#include "Veng/sh.slang"

[[vk::binding(0, 1)]] ByteAddressBuffer u_Coefficients; // 9 float4 (xyz coefficient, w pad)
[[vk::binding(1, 1)]] ByteAddressBuffer u_Normals;      // NormalCount float4 (xyz normal)
[[vk::binding(2, 1)]] RWStructuredBuffer<float4> u_Results; // per-normal irradiance (xyz)

struct ShEvalPush
{
    uint NormalCount;
};
[[vk::push_constant]] ShEvalPush u_Push;

[shader("compute")]
[numthreads(1, 1, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= u_Push.NormalCount)
    {
        return;
    }

    float4 coefficients[9];
    for (uint i = 0; i < 9; ++i)
    {
        coefficients[i] = u_Coefficients.Load<float4>(i * 16);
    }

    const float3 normal = u_Normals.Load<float4>(id.x * 16).xyz;
    u_Results[id.x] = float4(ShEvalIrradiance(coefficients, normal), 0.0);
}
