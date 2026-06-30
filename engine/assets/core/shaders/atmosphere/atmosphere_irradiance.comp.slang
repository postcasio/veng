// Ground irradiance LUT: for each (altitude r, sun-zenith cosine muS) texel, integrate the
// sky radiance (the precomputed scattering total) over the upper hemisphere to get the diffuse
// irradiance reaching a horizontal surface — the sky-driven ambient term Plan 07 consumes.
//
// Set 1: the scattering total (sampled 3D) + a shared sampler, and the destination 2D
// irradiance image (write-only storage). Off the bindless registry.
#include "atmosphere_common.slang"

[[vk::binding(0, 1)]] Texture3D<float4> g_Scattering;
[[vk::binding(1, 1)]] SamplerState g_Sampler;
[[vk::binding(2, 1)]] [format("rgba16f")] RWTexture2D<float4> g_Irradiance;

struct PushConstants
{
    AtmosphereParams Atmosphere;
};
[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

[shader("compute")]
[numthreads(8, 8, 1)]
void csMain(uint3 id : SV_DispatchThreadID)
{
    if (id.x >= uint(IrradianceWidth) || id.y >= uint(IrradianceHeight))
    {
        return;
    }
    AtmosphereParams a = g_PC.Atmosphere;
    float2 uv = (float2(id.xy) + 0.5) / float2(IrradianceWidth, IrradianceHeight);
    float r = a.PlanetRadius + uv.y * (a.AtmosphereRadius - a.PlanetRadius);
    float muS = ClampCosine(2.0 * uv.x - 1.0);

    float3 up = float3(0.0, 1.0, 0.0);

    // Cosine-weighted hemisphere integral of the sky radiance.
    const int Phi = 8;
    const int Theta = 4;
    float3 sum = float3(0.0);
    for (int t = 0; t < Theta; ++t)
    {
        float cosT = (float(t) + 0.5) / float(Theta); // upper hemisphere only
        float sinT = sqrt(max(1.0 - cosT * cosT, 0.0));
        for (int p = 0; p < Phi; ++p)
        {
            float phi = 2.0 * Pi * (float(p) + 0.5) / float(Phi);
            float3 w = float3(sinT * cos(phi), cosT, sinT * sin(phi));
            float muW = ClampCosine(dot(up, w));
            float3 uvw = ScatteringUvwFromParams(a, r, muW, muS);
            float3 radiance = g_Scattering.SampleLevel(g_Sampler, uvw, 0.0).rgb;
            sum += radiance * cosT;
        }
    }
    float3 irradiance = sum * (Pi / float(Phi * Theta)) * a.SunIrradiance;
    g_Irradiance[id.xy] = float4(irradiance, 1.0);
}
