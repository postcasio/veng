// Single scattering: for each (r, mu, muS) voxel, march the view ray and accumulate sunlight
// scattered once toward the viewer, attenuated by the transmittance to the viewer and to the
// sun (both read from the transmittance LUT). Rayleigh (wavelength-dependent) is stored in
// RGB and the grey Mie term in A; the phase functions are applied at sample time, so the
// table is azimuth-independent. This seeds the multiple-scattering iteration.
//
// Set 1: the transmittance LUT (sampled) + its sampler, and the destination 3D scattering
// volume (write-only storage), off the bindless registry.
#include "atmosphere_common.slang"

[[vk::binding(0, 1)]] Texture2D<float4> g_Transmittance;
[[vk::binding(1, 1)]] SamplerState g_Sampler;
[[vk::binding(2, 1)]] [format("rgba16f")] RWTexture3D<float4> g_Scattering;

struct PushConstants
{
    AtmosphereParams Atmosphere;
};
[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

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

    bool hitsGround = RayHitsGround(a, r, mu);
    float dist = hitsGround ? DistanceToGround(a, r, mu) : DistanceToTop(a, r, mu);

    const int Steps = 50;
    float dt = dist / float(Steps);

    float3 rayleigh = float3(0.0);
    float3 mie = float3(0.0);

    for (int i = 0; i < Steps; ++i)
    {
        float d = (float(i) + 0.5) * dt;
        float ri = clamp(sqrt(d * d + 2.0 * r * mu * d + r * r), a.PlanetRadius,
                         a.AtmosphereRadius);
        float h = ri - a.PlanetRadius;
        // Sun-zenith cosine at the sample point. The table is azimuth-independent (nu is not
        // tabulated), so the sun's radial component carries through the march unchanged.
        float muSi = ClampCosine((r * muS) / ri);

        // Transmittance viewer->sample (via the difference of two to-top lookups) and sample->sun.
        float3 tView = SampleTransmittanceToTop(a, g_Transmittance, g_Sampler, r, mu) /
                       max(SampleTransmittanceToTop(a, g_Transmittance, g_Sampler, ri,
                                                    ClampCosine((r * mu + d) / ri)),
                           float3(1e-6));
        tView = clamp(tView, 0.0, 1.0);
        float3 tSun = SampleTransmittanceToSun(a, g_Transmittance, g_Sampler, ri, muSi);
        float3 t = tView * tSun;

        float densR = exp(-h / a.RayleighHeight);
        float densM = exp(-h / a.MieHeight);
        rayleigh += t * densR * dt;
        mie += t * densM * dt;
    }

    rayleigh *= a.RayleighScattering;
    mie *= a.MieScattering;
    g_Scattering[id] = float4(rayleigh, mie.x);
}
