// Transmittance LUT: for each (altitude r, view-zenith cosine mu) texel, integrate the
// extinction from that point to the top of atmosphere and store exp(-opticalDepth) — the
// fraction of light surviving the path. The foundational table every later precompute pass
// samples for "light reaching this point through the air".
//
// Output is a 2D storage image bound on set 1 (a closed producer->consumer chain off the
// bindless registry).
#include "atmosphere_common.slang"

[[vk::binding(0, 1)]] [format("rgba16f")] RWTexture2D<float4> g_Transmittance;

struct PushConstants
{
    AtmosphereParams Atmosphere;
};
[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

float3 ComputeOpticalDepth(AtmosphereParams a, float r, float mu)
{
    const int Steps = 500;
    float dist = DistanceToTop(a, r, mu);
    float dt = dist / float(Steps);
    float3 depth = float3(0.0);
    for (int i = 0; i < Steps; ++i)
    {
        float d = (float(i) + 0.5) * dt;
        float ri = sqrt(max(d * d + 2.0 * r * mu * d + r * r, a.PlanetRadius * a.PlanetRadius));
        float h = ri - a.PlanetRadius;
        depth += Extinction(a, h) * dt;
    }
    return depth;
}

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= uint(TransmittanceWidth) || id.y >= uint(TransmittanceHeight))
    {
        return;
    }
    AtmosphereParams a = g_PC.Atmosphere;
    float2 uv = (float2(id.xy) + 0.5) / float2(TransmittanceWidth, TransmittanceHeight);
    float r;
    float mu;
    RMuFromTransmittanceUv(a, uv, r, mu);
    float3 t = exp(-ComputeOpticalDepth(a, r, mu));
    g_Transmittance[id.xy] = float4(t, 1.0);
}
