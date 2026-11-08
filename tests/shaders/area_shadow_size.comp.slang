// Evaluates the core area-shadow source-size policy (Veng/area_shadow.slang) for a set of
// (source radius, receiver→light distance, penumbra ratio) triples, so a test can pin the angular
// ratio and the cap without a shadow atlas, a light, or a rendered image. The whole point is
// sharing the exact include the deferred lighting pass uses — the policy decides whether the PCSS
// estimator is asked a question it can answer, and it is invisible in any output short of a lit
// scene. The include names no binding of its own, so the buffers here are the only ones bound; set
// 0 is reserved for the bindless registry, so they bind on set 3 as the other test shaders do.
#include "Veng/area_shadow.slang"
#include "Veng/binding.slang"

struct AreaShadowSizeSet3
{
    ByteAddressBuffer Cases;  // CaseCount float4 (radius, distance, penumbra, pad)
    RWStructuredBuffer<float4> Results;  // (sizeFactor, searchTexels, filterTexels, 0)
};
VE_DESCRIPTOR_SET(3, AreaShadowSizeSet3, g_Set3);

struct AreaShadowSizePush
{
    uint CaseCount;
};
VE_PUSH_CONSTANTS(AreaShadowSizePush, u_Push);

[shader("compute")]
[numthreads(1, 1, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= u_Push.CaseCount)
    {
        return;
    }

    const float4 c = g_Set3.Cases.Load<float4>(id.x * 16);
    const float sizeFactor = AreaShadowSourceSizeFactor(c.x, c.y);
    g_Set3.Results[id.x] = float4(sizeFactor, AreaShadowSearchTexels(sizeFactor),
                             AreaShadowFilterTexels(c.z, sizeFactor), 0.0);
}
