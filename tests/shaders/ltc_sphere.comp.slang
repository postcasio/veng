// Evaluates the shipped Sphere-light LTC integrator (Veng/ltc.slang), so a test can pin it without a
// g-buffer, a light buffer, or a rendered image. It needs none of the deferred pass's bindings — only
// the include the pass itself uses — and runs in one of two modes, chosen by the push block:
//
// - Diffuse (Mode 0): one case per source angular size, through the identity transform. A uniform
//   sphere of radius r at distance d irradiates a facing Lambertian surface with a clamped-cosine
//   form factor of exactly sin^2(theta), sin(theta) = r/d — independent of whether the emitter is
//   the sphere or its silhouette disk, since both share the same projected solid angle. So
//   LtcSphere's diffuse answer must track sin^2(theta) across the whole angular range.
// - Specular (Mode 1): one case per view direction and fitted inverse transform (the LUT's matrix,
//   handed in rather than sampled, so no LUT binding is needed). Each case sweeps a grid of sphere
//   centres wholly below the surface's plane, which must light nothing, and evaluates the sphere at
//   the view's mirror direction, which must reflect.
//
// set 0 is reserved for the bindless registry, so the buffers bind on set 3 as the other test
// shaders do.
#include "Veng/ltc.slang"
#include "Veng/binding.slang"

// Diffuse: CaseCount float4 (rOverD, pad, pad, pad). Specular: CaseCount × 3 float4 — the view
// direction (xyz), the inverse transform's top row (m00, 0, m02) and its bottom row (m20, 0, m22).
struct LtcSphereSet3
{
    ByteAddressBuffer Cases;
    // Diffuse: (diffSum, sin2theta, 0, 0). Specular: (largest sum from below the plane, mirror sum, 0, 0).
    RWStructuredBuffer<float4> Results;
};
VE_DESCRIPTOR_SET(3, LtcSphereSet3, g_Set3);

struct LtcSpherePush
{
    uint CaseCount;
    uint Mode;
};
VE_PUSH_CONSTANTS(LtcSpherePush, u_Push);

static const float Pi = 3.14159265;

void Diffuse(uint index)
{
    const float rOverD = g_Set3.Cases.Load<float4>(index * 16).x;

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
    g_Set3.Results[index] = float4(diffSum, rOverD * rOverD, 0.0, 0.0);
}

void Specular(uint index)
{
    const float3 V = g_Set3.Cases.Load<float4>(index * 48).xyz;
    const float4 top = g_Set3.Cases.Load<float4>(index * 48 + 16);
    const float4 bottom = g_Set3.Cases.Load<float4>(index * 48 + 32);
    const float3x3 Minv = float3x3(top.x, 0.0, top.z, 0.0, 1.0, 0.0, bottom.x, 0.0, bottom.z);

    const float3 P = float3(0.0, 0.0, 0.0);
    const float3 N = float3(0.0, 0.0, 1.0);
    const float d = 1000.0;

    // Centres from just below the plane to straight down, all round, for a star-sized source and a
    // larger one; the elevation keeps every sphere wholly below the plane.
    float behind = 0.0;
    const float rOverD[2] = { 0.041, 0.2 };
    for (uint s = 0; s < 2; ++s)
    {
        const float sinTheta = rOverD[s];
        const float theta = asin(sinTheta);
        for (uint az = 0; az < 48; ++az)
        {
            const float phi = float(az) * (2.0 * Pi / 48.0);
            for (uint el = 0; el < 12; ++el)
            {
                const float e = -(theta + 0.001 + (0.5 * Pi - theta - 0.002) * float(el) / 11.0);
                const float3 dir = float3(cos(e) * cos(phi), cos(e) * sin(phi), sin(e));
                behind = max(behind, LtcSphere(N, V, P, Minv, dir * d, sinTheta * d));
            }
        }
    }

    const float3 mirror = reflect(-V, N);
    const float reflected = LtcSphere(N, V, P, Minv, mirror * d, 0.041 * d);
    g_Set3.Results[index] = float4(behind, reflected, 0.0, 0.0);
}

[shader("compute")]
[numthreads(1, 1, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= u_Push.CaseCount)
    {
        return;
    }
    if (u_Push.Mode == 0u)
    {
        Diffuse(id.x);
    }
    else
    {
        Specular(id.x);
    }
}
