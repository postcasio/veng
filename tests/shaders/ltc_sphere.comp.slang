// Evaluates the shipped Sphere-light LTC integrator (Veng/ltc.slang) for a set of source angular
// sizes, so a test can pin its absolute magnitude against the analytic sphere irradiance without a
// g-buffer, a light buffer, or a rendered image. The diffuse path uses the identity transform, so
// it needs none of the deferred pass's LUT or bindings — only the include the pass itself uses.
//
// A uniform sphere of radius r at distance d irradiates a facing Lambertian surface with a
// clamped-cosine form factor of exactly sin^2(theta), sin(theta) = r/d — independent of whether the
// emitter is the sphere or its silhouette disk, since both share the same projected solid angle. So
// LtcSphere's diffuse answer must track sin^2(theta) across the whole angular range; the test reads
// that relation off the shader's own numbers. set 0 is reserved for the bindless registry, so the
// buffers bind on set 3 as the other test shaders do.
#include "Veng/ltc.slang"

[[vk::binding(0, 3)]] ByteAddressBuffer u_Cases;             // CaseCount float4 (rOverD, pad, pad, pad)
[[vk::binding(1, 3)]] RWStructuredBuffer<float4> u_Results;  // (diffSum, sin2theta, 0, 0)

struct LtcSpherePush
{
    uint CaseCount;
};
[[vk::push_constant]] LtcSpherePush u_Push;

[shader("compute")]
[numthreads(1, 1, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= u_Push.CaseCount)
    {
        return;
    }

    const float rOverD = u_Cases.Load<float4>(id.x * 16).x;

    // The shading point faces the sphere head-on: N points at the centre, so the clamped-cosine
    // form factor is at its analytic maximum sin^2(theta). V is offset off N only to give the LTC a
    // well-defined tangent frame; the identity-transform diffuse answer does not depend on it.
    const float d = 1000.0;
    const float r = rOverD * d;
    const float3 P = float3(0.0, 0.0, 0.0);
    const float3 center = float3(0.0, 0.0, d);
    const float3 N = float3(0.0, 0.0, 1.0);
    const float3 V = normalize(float3(0.0, 1.0, 0.2));
    const float3x3 ident = float3x3(1.0, 0.0, 0.0, 0.0, 1.0, 0.0, 0.0, 0.0, 1.0);

    const float diffSum = LtcSphere(N, V, P, ident, center, r);
    u_Results[id.x] = float4(diffSum, rOverD * rOverD, 0.0, 0.0);
}
