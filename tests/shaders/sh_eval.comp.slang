// Evaluates the core GPU spherical-harmonics irradiance reconstruction (Veng/sh.slang's
// ShEvalIrradiance) for a set of normals against one coefficient set, so a test can compare
// the GPU result to the CPU EvalIrradiance and the checked-in golden constants. The whole
// point is sharing the exact basis include with the deferred lighting pass — this shader
// pulls in the same Veng/sh.slang the engine does, so any basis/normalization drift between
// CPU and GPU fails the consistency test. Sets 0-2 are reserved for the bindless registries, so
// the buffers bind on set 3, matching the test's hand-built layout.
#include "Veng/sh.slang"
#include "Veng/binding.slang"

struct ShEvalSet3
{
    ByteAddressBuffer Coefficients;  // 9 float4 (xyz coefficient, w pad)
    ByteAddressBuffer Normals;  // NormalCount float4 (xyz normal)
    RWStructuredBuffer<float4> Results;  // per-normal irradiance (xyz)
};
VE_DESCRIPTOR_SET(3, ShEvalSet3, g_Set3);

struct ShEvalPush
{
    uint NormalCount;
};
VE_PUSH_CONSTANTS(ShEvalPush, u_Push);

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
        coefficients[i] = g_Set3.Coefficients.Load<float4>(i * 16);
    }

    const float3 normal = g_Set3.Normals.Load<float4>(id.x * 16).xyz;
    g_Set3.Results[id.x] = float4(ShEvalIrradiance(coefficients, normal), 0.0);
}
