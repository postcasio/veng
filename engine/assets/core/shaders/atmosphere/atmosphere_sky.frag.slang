// Atmosphere sky: fills the cleared-depth background with the precomputed atmospheric
// scattering sampled along each pixel's view ray, plus the sun disk, compositing over the lit
// scene color. Occupies the same slot as the cubemap skybox — a fullscreen pass after
// deferred lighting, before the bloom/SSR/tonemap tail — so the procedural sky and the cubemap
// skybox are interchangeable sky sources in one place.
//
// The depth target is sampled through set-0 bindless; the scattering total + transmittance
// LUTs + a shared sampler ride the renderer's atmosphere set (set 1 here). The sun direction
// and atmosphere parameters ride the push.
#include "../view_constants.slang"
#include "atmosphere_common.slang"

[[vk::binding(0, 0)]] Texture2D g_Textures[];
[[vk::binding(1, 0)]] SamplerState g_Samplers[];

[[vk::binding(0, 1)]] Texture3D<float4> g_Scattering;
[[vk::binding(1, 1)]] Texture2D<float4> g_Transmittance;
[[vk::binding(2, 1)]] SamplerState g_AtmosphereSampler;

struct PushConstants
{
    uint DepthTexture;       // g-buffer depth bindless index
    uint Sampler;            // shared sampler bindless index
    uint ViewConstantsIndex; // current frame-in-flight's view-constants region
    uint Enabled;            // 1 = render the atmosphere this frame; 0 = discard all
    float3 SunDirection;     // normalized direction toward the sun (world up +Y)
    float Intensity;         // scales the sky radiance + sun disk
    AtmosphereParams Atmosphere;
};
[[vk::push_constant]] ConstantBuffer<PushConstants> g_PC;

struct VSOutput
{
    float4 sv_position : SV_Position;
    float2 v_UV : TEXCOORD0;
};

// Sky radiance along a view direction from a viewer at world up +Y, reconstructed from the
// scattering total (Rayleigh in RGB, Mie scalar in A) with the phase functions reapplied.
float3 SkyRadiance(AtmosphereParams a, float3 viewDir, float3 sunDir, float viewAltitude)
{
    float r = a.PlanetRadius + max(viewAltitude, 0.0);
    float3 up = float3(0.0, 1.0, 0.0);
    float mu = ClampCosine(dot(up, viewDir));
    float muS = ClampCosine(dot(up, sunDir));
    float nu = dot(viewDir, sunDir);

    float3 uvw = ScatteringUvwFromParams(a, r, mu, muS);
    float4 s = g_Scattering.SampleLevel(g_AtmosphereSampler, uvw, 0.0);

    float rayleighPhase = (3.0 / (16.0 * Pi)) * (1.0 + nu * nu);
    float g = a.MieAnisotropy;
    float miePhase = (1.0 - g * g) / (4.0 * Pi * pow(max(1.0 + g * g - 2.0 * g * nu, 1e-4), 1.5));

    float3 rayleigh = s.rgb * rayleighPhase;
    float3 mie = a.MieScattering * (s.a / max(a.MieScattering.x, 1e-9)) * miePhase;
    return (rayleigh + mie) * a.SunIrradiance;
}

[shader("fragment")]
float4 fsMain(VSOutput input) : SV_Target
{
    if (g_PC.Enabled == 0)
    {
        discard;
    }

    AtmosphereParams a = g_PC.Atmosphere;
    SamplerState samp = g_Samplers[NonUniformResourceIndex(g_PC.Sampler)];
    ViewConstants vc = LoadViewConstants(g_PC.ViewConstantsIndex);

    float2 sampleUV = ScaledSampleUV(input.v_UV, vc.RenderScaleUV.xy, vc.MaxValidUV.xy);
    float depth = g_Textures[NonUniformResourceIndex(g_PC.DepthTexture)].Sample(samp, sampleUV).r;

    // Geometry already shaded this pixel; leave the lit color untouched.
    if (depth < 1.0)
    {
        discard;
    }

    // Unproject the far-plane point through the inverse view-projection and form the view ray.
    float2 ndcXY = input.v_UV * 2.0 - 1.0;
    float4 worldH = mul(vc.InvViewProj, float4(ndcXY, 1.0, 1.0));
    float3 worldPos = worldH.xyz / worldH.w;
    float3 dir = normalize(worldPos - vc.CameraPosition.xyz);
    float3 sunDir = normalize(g_PC.SunDirection);

    float3 sky = SkyRadiance(a, dir, sunDir, 0.5);

    // Sun disk: the extra-terrestrial irradiance attenuated by the transmittance along the
    // view ray, with a soft antialiased edge over the angular radius.
    float cosAngle = dot(dir, sunDir);
    float disk = smoothstep(cos(a.SunAngularRadius * 1.5), cos(a.SunAngularRadius), cosAngle);
    if (disk > 0.0)
    {
        float r = a.PlanetRadius + 0.5;
        float mu = ClampCosine(dir.y);
        float3 t = SampleTransmittanceToTop(a, g_Transmittance, g_AtmosphereSampler, r, mu);
        sky += t * a.SunIrradiance * 20.0 * disk;
    }

    return float4(sky * g_PC.Intensity, 1.0);
}
