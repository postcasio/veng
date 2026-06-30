// Shared atmosphere model + Bruneton-style LUT parameterization for the precompute and
// the runtime sky eval. Lengths are in kilometers, scattering coefficients in 1/km — the
// scale the CPU Atmosphere struct (Veng/Renderer/Atmosphere.h) uses, so the two stay in
// lockstep. No bindings here; each shader binds its own storage/sampled images on set 1.

static const float Pi = 3.14159265358979323846;

// The 4D scattering table dimensions, packed into a 3D texture as
// width = MuSRes * NuRes, height = MuRes, depth = RRes (the Bruneton layout).
static const int TransmittanceWidth = 256;
static const int TransmittanceHeight = 64;
static const int ScatteringRRes = 32;
static const int ScatteringMuRes = 128;
static const int ScatteringMuSRes = 32;
static const int ScatteringNuRes = 8;
static const int IrradianceWidth = 64;
static const int IrradianceHeight = 16;

// The atmosphere parameters, mirroring Veng::Renderer::Atmosphere field-for-field. Pushed
// to every precompute dispatch and to the runtime sky pass so the tables and the eval read
// identical constants.
struct AtmosphereParams
{
    float3 RayleighScattering;
    float RayleighHeight;
    float3 MieScattering;
    float MieExtinction;
    float3 OzoneAbsorption;
    float MieHeight;
    float MieAnisotropy;
    float OzoneCenter;
    float OzoneWidth;
    float PlanetRadius;
    float AtmosphereRadius;
    float SunAngularRadius;
    float Pad0;
    float Pad1;
    float3 SunIrradiance;
    float Pad2;
};

// Combined per-channel extinction at altitude h (km above the surface).
float3 Extinction(AtmosphereParams a, float h)
{
    float rayleigh = exp(-max(h, 0.0) / a.RayleighHeight);
    float mie = exp(-max(h, 0.0) / a.MieHeight);
    float ozone = max(0.0, 1.0 - abs(h - a.OzoneCenter) / a.OzoneWidth);
    float mieRatio = a.MieExtinction / max(a.MieScattering.x, 1e-9);
    return a.RayleighScattering * rayleigh + a.MieScattering * (mieRatio * mie) +
           a.OzoneAbsorption * ozone;
}

// Distance to the top-of-atmosphere along a ray from radius r with view-zenith cosine mu.
float DistanceToTop(AtmosphereParams a, float r, float mu)
{
    float disc = r * r * (mu * mu - 1.0) + a.AtmosphereRadius * a.AtmosphereRadius;
    return max(-r * mu + sqrt(max(disc, 0.0)), 0.0);
}

// Distance to the ground along a ray, or a negative sentinel when the ray misses the planet.
float DistanceToGround(AtmosphereParams a, float r, float mu)
{
    float disc = r * r * (mu * mu - 1.0) + a.PlanetRadius * a.PlanetRadius;
    if (disc < 0.0)
    {
        return -1.0;
    }
    return max(-r * mu - sqrt(disc), 0.0);
}

bool RayHitsGround(AtmosphereParams a, float r, float mu)
{
    return mu < 0.0 && (r * r * (mu * mu - 1.0) + a.PlanetRadius * a.PlanetRadius) >= 0.0;
}

float ClampCosine(float x) { return clamp(x, -1.0, 1.0); }

// --- Transmittance LUT (r, mu) <-> texture UV ---------------------------------------------

float2 TransmittanceUvFromRMu(AtmosphereParams a, float r, float mu)
{
    float H = sqrt(max(a.AtmosphereRadius * a.AtmosphereRadius - a.PlanetRadius * a.PlanetRadius,
                       0.0));
    float rho = sqrt(max(r * r - a.PlanetRadius * a.PlanetRadius, 0.0));
    float d = DistanceToTop(a, r, mu);
    float dMin = a.AtmosphereRadius - r;
    float dMax = rho + H;
    float xMu = (dMax - dMin) > 0.0 ? (d - dMin) / (dMax - dMin) : 0.0;
    float xR = H > 0.0 ? rho / H : 0.0;
    return float2(clamp(xMu, 0.0, 1.0), clamp(xR, 0.0, 1.0));
}

void RMuFromTransmittanceUv(AtmosphereParams a, float2 uv, out float r, out float mu)
{
    float H = sqrt(max(a.AtmosphereRadius * a.AtmosphereRadius - a.PlanetRadius * a.PlanetRadius,
                       0.0));
    float rho = H * uv.y;
    r = sqrt(rho * rho + a.PlanetRadius * a.PlanetRadius);
    float dMin = a.AtmosphereRadius - r;
    float dMax = rho + H;
    float d = dMin + uv.x * (dMax - dMin);
    mu = d == 0.0 ? 1.0 : (H * H - rho * rho - d * d) / (2.0 * r * d);
    mu = ClampCosine(mu);
}

// Samples the transmittance LUT for the path from radius r, view cosine mu, to the top of
// atmosphere. The caller passes the sampled LUT + its sampler.
float3 SampleTransmittanceToTop(AtmosphereParams a, Texture2D<float4> lut, SamplerState samp,
                                float r, float mu)
{
    float2 uv = TransmittanceUvFromRMu(a, r, mu);
    return lut.SampleLevel(samp, uv, 0.0).rgb;
}

// Transmittance from point at (r, mu) to the sun direction with sun-zenith cosine muS,
// accounting for ground shadowing — the standard product of two top-of-atmosphere lookups.
float3 SampleTransmittanceToSun(AtmosphereParams a, Texture2D<float4> lut, SamplerState samp,
                                float r, float muS)
{
    // Soft horizon: the sun's angular radius lets a sliver of light past the geometric horizon.
    float sinH = a.PlanetRadius / r;
    float cosH = -sqrt(max(1.0 - sinH * sinH, 0.0));
    float visible = smoothstep(-sinH * a.SunAngularRadius, sinH * a.SunAngularRadius, muS - cosH);
    return SampleTransmittanceToTop(a, lut, samp, r, muS) * visible;
}

// --- Scattering 3D table (r, mu, muS) <-> packed 3D texel ---------------------------------
//
// The scattering table is parameterized on altitude r, view-zenith cosine mu, and sun-zenith
// cosine muS, stored as a 3D texture: z = r slice (ScatteringRRes), y = mu (ScatteringMuRes),
// x = muS (ScatteringMuSRes). The view-sun azimuth (nu) is not tabulated; the phase functions
// are applied at sample time, the standard real-time simplification of Bruneton's 4D table —
// it keeps the volume tractable while preserving the day/night, horizon, and altitude behavior.
// Rayleigh and Mie are stored together (RGB = Rayleigh, A would lose chroma), so single and
// multiple scattering each keep a full RGB Rayleigh term and a grey Mie term folded in.

static const int3 ScatteringSize =
    int3(ScatteringMuSRes, ScatteringMuRes, ScatteringRRes);

// Maps a 0.5-centered texel coordinate to a [0,1] texture coordinate avoiding the edge.
float TexCoordFromUnit(float u, int size)
{
    return 0.5 / float(size) + u * (1.0 - 1.0 / float(size));
}
float UnitFromTexCoord(float t, int size)
{
    return (t - 0.5 / float(size)) / (1.0 - 1.0 / float(size));
}

void ScatteringParamsFromTexel(AtmosphereParams a, int3 texel, out float r, out float mu,
                               out float muS)
{
    float H = sqrt(max(a.AtmosphereRadius * a.AtmosphereRadius - a.PlanetRadius * a.PlanetRadius,
                       0.0));
    float xR = UnitFromTexCoord((float(texel.z) + 0.5) / float(ScatteringRRes), ScatteringRRes);
    float rho = H * xR;
    r = clamp(sqrt(rho * rho + a.PlanetRadius * a.PlanetRadius), a.PlanetRadius,
              a.AtmosphereRadius);

    float xMu = UnitFromTexCoord((float(texel.y) + 0.5) / float(ScatteringMuRes), ScatteringMuRes);
    mu = ClampCosine(2.0 * xMu - 1.0);

    float xMuS =
        UnitFromTexCoord((float(texel.x) + 0.5) / float(ScatteringMuSRes), ScatteringMuSRes);
    muS = ClampCosine(2.0 * xMuS - 1.0);
}

float3 ScatteringUvwFromParams(AtmosphereParams a, float r, float mu, float muS)
{
    float H = sqrt(max(a.AtmosphereRadius * a.AtmosphereRadius - a.PlanetRadius * a.PlanetRadius,
                       0.0));
    float rho = sqrt(max(r * r - a.PlanetRadius * a.PlanetRadius, 0.0));
    float xR = H > 0.0 ? rho / H : 0.0;
    float xMu = mu * 0.5 + 0.5;
    float xMuS = muS * 0.5 + 0.5;
    return float3(TexCoordFromUnit(xMuS, ScatteringMuSRes),
                  TexCoordFromUnit(xMu, ScatteringMuRes),
                  TexCoordFromUnit(xR, ScatteringRRes));
}

