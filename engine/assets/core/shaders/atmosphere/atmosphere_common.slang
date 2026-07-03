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
// x = muS (ScatteringMuSRes). The three axes use Bruneton's non-linear mappings so texel
// resolution follows where the sky varies fastest:
//   r   maps through rho/H, the horizon distance over its maximum.
//   mu  is horizon-split: the lower half of the axis holds ground-intersecting rays (mapped by
//       distance-to-ground), the upper half holds sky rays (mapped by distance-to-top). The two
//       halves meet at the horizon with a half-texel inset each, so a sample never interpolates
//       across the ground/sky discontinuity.
//   muS is warped to pack most texels near muS = 0, the horizon band where the sunset ramp from
//       red to blue lives; a linear muS averages that whole ramp into a few texels and greys it.
// The view-sun azimuth (nu) is not tabulated; the phase functions are applied at sample time,
// the standard real-time reduction of Bruneton's 4D table, preserving the day/night, horizon,
// and altitude behavior. Rayleigh and Mie are stored together (RGB = Rayleigh, A the grey Mie
// term), so single and multiple scattering each keep a full RGB Rayleigh term and a grey Mie
// term folded in.

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

// Sun-zenith cosine below which the sky is dark; Bruneton's mu_s_min = cos(102 deg). The muS
// mapping warps around this floor to spend most texels near the horizon.
static const float ScatteringMuSMin = -0.2079116908;

// Decodes a scattering texel to (r, mu, muS) and reports whether the mu row is a
// ground-intersecting ray. rayHitsGround comes from the row's half rather than a re-derivation,
// so a precompute march uses exactly the endpoint its texel's mu was encoded against.
void ScatteringParamsFromTexel(AtmosphereParams a, int3 texel, out float r, out float mu,
                               out float muS, out bool rayHitsGround)
{
    float H = sqrt(max(a.AtmosphereRadius * a.AtmosphereRadius - a.PlanetRadius * a.PlanetRadius,
                       0.0));
    float xR = UnitFromTexCoord((float(texel.z) + 0.5) / float(ScatteringRRes), ScatteringRRes);
    float rho = H * xR;
    r = clamp(sqrt(rho * rho + a.PlanetRadius * a.PlanetRadius), a.PlanetRadius,
              a.AtmosphereRadius);

    float uMu = (float(texel.y) + 0.5) / float(ScatteringMuRes);
    if (uMu < 0.5)
    {
        // Lower half: ground rays, distance-to-ground over [r - Rp, rho].
        float dMin = r - a.PlanetRadius;
        float dMax = rho;
        float d = dMin + (dMax - dMin) * UnitFromTexCoord(1.0 - 2.0 * uMu, ScatteringMuRes / 2);
        mu = d == 0.0 ? -1.0 : ClampCosine(-(rho * rho + d * d) / (2.0 * r * d));
        rayHitsGround = true;
    }
    else
    {
        // Upper half: sky rays, distance-to-top over [Ra - r, rho + H].
        float dMin = a.AtmosphereRadius - r;
        float dMax = rho + H;
        float d = dMin + (dMax - dMin) * UnitFromTexCoord(2.0 * uMu - 1.0, ScatteringMuRes / 2);
        mu = d == 0.0 ? 1.0 : ClampCosine((H * H - rho * rho - d * d) / (2.0 * r * d));
        rayHitsGround = false;
    }

    float xMuS = UnitFromTexCoord((float(texel.x) + 0.5) / float(ScatteringMuSRes),
                                  ScatteringMuSRes);
    float dMinS = a.AtmosphereRadius - a.PlanetRadius;
    float dMaxS = H;
    float D = DistanceToTop(a, a.PlanetRadius, ScatteringMuSMin);
    float A = (D - dMinS) / (dMaxS - dMinS);
    float aVal = (A - xMuS * A) / (1.0 + xMuS * A);
    float dS = dMinS + min(aVal, A) * (dMaxS - dMinS);
    muS = dS == 0.0 ? 1.0 : ClampCosine((H * H - dS * dS) / (2.0 * a.PlanetRadius * dS));
}

// Encodes (r, mu, muS) to a scattering texture coordinate. rayHitsGround selects the mu half so
// the coordinate lands on the same side of the horizon split the parameters describe; it is the
// exact inverse of ScatteringParamsFromTexel.
float3 ScatteringUvwFromParams(AtmosphereParams a, float r, float mu, float muS,
                               bool rayHitsGround)
{
    float H = sqrt(max(a.AtmosphereRadius * a.AtmosphereRadius - a.PlanetRadius * a.PlanetRadius,
                       0.0));
    float rho = sqrt(max(r * r - a.PlanetRadius * a.PlanetRadius, 0.0));
    float uR = TexCoordFromUnit(H > 0.0 ? rho / H : 0.0, ScatteringRRes);

    float rMu = r * mu;
    float discriminant = rMu * rMu - r * r + a.PlanetRadius * a.PlanetRadius;
    float uMu;
    if (rayHitsGround)
    {
        float d = -rMu - sqrt(max(discriminant, 0.0));
        float dMin = r - a.PlanetRadius;
        float dMax = rho;
        float x = dMax == dMin ? 0.0 : (d - dMin) / (dMax - dMin);
        uMu = 0.5 - 0.5 * TexCoordFromUnit(x, ScatteringMuRes / 2);
    }
    else
    {
        float d = -rMu + sqrt(max(discriminant + H * H, 0.0));
        float dMin = a.AtmosphereRadius - r;
        float dMax = rho + H;
        float x = (dMax - dMin) > 0.0 ? (d - dMin) / (dMax - dMin) : 0.0;
        uMu = 0.5 + 0.5 * TexCoordFromUnit(x, ScatteringMuRes / 2);
    }

    float dS = DistanceToTop(a, a.PlanetRadius, muS);
    float dMinS = a.AtmosphereRadius - a.PlanetRadius;
    float dMaxS = H;
    float aVal = (dS - dMinS) / (dMaxS - dMinS);
    float D = DistanceToTop(a, a.PlanetRadius, ScatteringMuSMin);
    float A = (D - dMinS) / (dMaxS - dMinS);
    float uMuS = TexCoordFromUnit(max(1.0 - aVal / A, 0.0) / (1.0 + aVal), ScatteringMuSRes);

    return float3(uMuS, uMu, uR);
}

