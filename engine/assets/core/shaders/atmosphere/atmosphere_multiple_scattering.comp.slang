// Multiple-scattering iteration: advances the accumulated scattering by one bounce. For each
// (r, mu, muS) voxel it gathers the light already scattered (the running total so far, read
// from g_PrevTotal) arriving from a set of directions over the sphere, scatters it once more
// toward the viewer along the marched view ray, and adds that new bounce onto the running
// total. g_PrevTotal is the sampled source; g_NextTotal is the write-only destination — the
// ping-pong the iteration runs over (never an in-place read-write of one 3D image).
//
// Set 1: the running-total scattering (sampled 3D) + the transmittance LUT (sampled 2D) + a
// shared sampler, and the next-total destination 3D volume (write-only storage). Off bindless.
#include "atmosphere_common.slang"

[[vk::binding(0, 1)]] Texture3D<float4> g_PrevTotal;
[[vk::binding(1, 1)]] Texture2D<float4> g_Transmittance;
[[vk::binding(2, 1)]] SamplerState g_Sampler;
[[vk::binding(3, 1)]] [format("rgba16f")] RWTexture3D<float4> g_NextTotal;

struct PushConstants
{
    AtmosphereParams Atmosphere;
};
[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

// Total in-scattered radiance arriving at radius r from direction w, given the sun cosine muS,
// reconstructed from the order-n table with its phase functions reapplied.
float3 GatherRadiance(AtmosphereParams a, float r, float3 viewW, float muS, float3 up,
                      float3 sunDir)
{
    // Sample a small spherical set of incoming directions and average their order-n radiance.
    const int Phi = 8;
    const int Theta = 4;
    float3 sum = float3(0.0);
    float weight = 0.0;
    for (int t = 0; t < Theta; ++t)
    {
        float cosT = 1.0 - 2.0 * (float(t) + 0.5) / float(Theta);
        float sinT = sqrt(max(1.0 - cosT * cosT, 0.0));
        for (int p = 0; p < Phi; ++p)
        {
            float phi = 2.0 * Pi * (float(p) + 0.5) / float(Phi);
            float3 w = float3(sinT * cos(phi), cosT, sinT * sin(phi));
            float muW = ClampCosine(dot(up, w));
            float3 uvw = ScatteringUvwFromParams(a, r, muW, muS);
            float4 s = g_PrevTotal.SampleLevel(g_Sampler, uvw, 0.0);
            float nu = dot(w, sunDir);
            float phase = (3.0 / (16.0 * Pi)) * (1.0 + nu * nu);
            sum += s.rgb * phase;
            weight += 1.0;
        }
    }
    return weight > 0.0 ? sum * (4.0 * Pi / weight) : float3(0.0);
}

[shader("compute")]
[numthreads(4, 4, 4)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (any(id >= uint3(ScatteringSize)))
    {
        return;
    }
    AtmosphereParams a = g_PC.Atmosphere;
    float r;
    float mu;
    float muS;
    ScatteringParamsFromTexel(a, int3(id), r, mu, muS);

    // Local frame at the voxel: up is radial; the sun lies in the up/forward plane at cosine muS.
    float3 up = float3(0.0, 1.0, 0.0);
    float3 sunDir = float3(sqrt(max(1.0 - muS * muS, 0.0)), muS, 0.0);
    float3 viewW = float3(sqrt(max(1.0 - mu * mu, 0.0)), mu, 0.0);

    bool hitsGround = RayHitsGround(a, r, mu);
    float dist = hitsGround ? DistanceToGround(a, r, mu) : DistanceToTop(a, r, mu);
    const int Steps = 16;
    float dt = dist / float(Steps);

    float3 rayleigh = float3(0.0);
    for (int i = 0; i < Steps; ++i)
    {
        float d = (float(i) + 0.5) * dt;
        float ri = clamp(sqrt(d * d + 2.0 * r * mu * d + r * r), a.PlanetRadius,
                         a.AtmosphereRadius);
        float h = ri - a.PlanetRadius;
        float muSi = ClampCosine((r * muS) / ri);

        float3 tView = SampleTransmittanceToTop(a, g_Transmittance, g_Sampler, r, mu) /
                       max(SampleTransmittanceToTop(a, g_Transmittance, g_Sampler, ri,
                                                    ClampCosine((r * mu + d) / ri)),
                           float3(1e-6));
        tView = clamp(tView, 0.0, 1.0);

        float3 gathered = GatherRadiance(a, ri, viewW, muSi, up, sunDir);
        float densR = exp(-h / a.RayleighHeight);
        rayleigh += tView * gathered * densR * dt;
    }

    rayleigh *= a.RayleighScattering;

    // Add this bounce onto the running total (Rayleigh chroma in RGB, the seeded Mie term in A
    // carried through unchanged — higher-order Mie is negligible).
    float4 prev = g_PrevTotal[id];
    g_NextTotal[id] = float4(prev.rgb + rayleigh, prev.a);
}
