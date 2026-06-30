// Device-free atmosphere CPU eval: the single-scattering reference is pure glm math, no
// Context, no Vulkan. It must reproduce the qualitative day/night and horizon behavior the
// GPU LUTs precompute — a noon sky brighter and bluer than the horizon, the horizon warmer
// (more red-shifted), and a sun below the horizon giving near-black — so the parameterization
// is pinned without a golden image.

#include <doctest/doctest.h>

#include <Veng/Renderer/Atmosphere.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    f32 Luminance(const vec3& c)
    {
        return 0.2126f * c.r + 0.7152f * c.g + 0.0722f * c.b;
    }
}

TEST_CASE("Ray-sphere returns the far intersection")
{
    const Atmosphere a;
    const vec3 origin(0.0f, a.PlanetRadius + 1.0f, 0.0f);
    // Straight up hits the top of atmosphere at AtmosphereRadius - origin.y.
    const f32 up = AtmosphereRaySphere(origin, vec3(0.0f, 1.0f, 0.0f), a.AtmosphereRadius);
    CHECK(up == doctest::Approx(a.AtmosphereRadius - origin.y).epsilon(0.01));
    // A ray pointing away from a sphere it is inside still has a positive far root.
    CHECK(up > 0.0f);
    // A small sphere the origin is far outside, pointed away, misses.
    CHECK(AtmosphereRaySphere(vec3(0.0f, 100.0f, 0.0f), vec3(0.0f, 1.0f, 0.0f), 1.0f) == 0.0f);
}

TEST_CASE("Phase functions are normalized and oriented")
{
    // Rayleigh is symmetric: forward and backward scatter equally, more than sideways.
    CHECK(AtmosphereRayleighPhase(1.0f) == doctest::Approx(AtmosphereRayleighPhase(-1.0f)));
    CHECK(AtmosphereRayleighPhase(1.0f) > AtmosphereRayleighPhase(0.0f));

    // Mie with positive g is strongly forward-scattering.
    CHECK(AtmosphereMiePhase(1.0f, 0.8f) > AtmosphereMiePhase(-1.0f, 0.8f));
}

TEST_CASE("Densities fall off with altitude")
{
    const Atmosphere a;
    const AtmosphereDensity ground = AtmosphereDensities(a, 0.0f);
    const AtmosphereDensity high = AtmosphereDensities(a, 20.0f);
    CHECK(ground.Rayleigh == doctest::Approx(1.0f));
    CHECK(ground.Mie == doctest::Approx(1.0f));
    CHECK(high.Rayleigh < ground.Rayleigh);
    CHECK(high.Mie < ground.Mie);
    // Ozone peaks at its layer center, not the ground.
    CHECK(AtmosphereDensities(a, a.OzoneCenter).Ozone > ground.Ozone);
}

TEST_CASE("Sky radiance is plausible across sun elevations")
{
    const Atmosphere a;
    const vec3 zenith(0.0f, 1.0f, 0.0f);
    const vec3 horizon = glm::normalize(vec3(1.0f, 0.05f, 0.0f));

    const vec3 sunHigh = glm::normalize(vec3(0.2f, 1.0f, 0.0f));   // near noon
    const vec3 sunBelow = glm::normalize(vec3(0.2f, -0.3f, 0.0f)); // below the horizon (night)

    const vec3 noonZenith = AtmosphereSkyRadiance(a, zenith, sunHigh);
    const vec3 noonHorizon = AtmosphereSkyRadiance(a, horizon, sunHigh);
    const vec3 night = AtmosphereSkyRadiance(a, zenith, sunBelow);

    // The daytime sky is positive everywhere it integrates.
    CHECK(Luminance(noonZenith) > 0.0f);

    // Noon zenith is bluer than it is red (Rayleigh's blue bias).
    CHECK(noonZenith.b > noonZenith.r);

    // The horizon is warmer than the zenith: a longer air path scatters out more blue, so the
    // red/blue ratio at the horizon exceeds the zenith's.
    const f32 zenithRatio = noonZenith.r / std::max(noonZenith.b, 1e-6f);
    const f32 horizonRatio = noonHorizon.r / std::max(noonHorizon.b, 1e-6f);
    CHECK(horizonRatio > zenithRatio);

    // A sun below the horizon yields a near-dark sky (orders of magnitude below noon).
    CHECK(Luminance(night) < Luminance(noonZenith) * 0.1f);
}

TEST_CASE("Optical depth grows with path length")
{
    const Atmosphere a;
    const vec3 origin(0.0f, a.PlanetRadius + 0.1f, 0.0f);
    // A grazing ray travels far more air than a straight-up ray, so its optical depth is larger.
    const vec3 up = AtmosphereOpticalDepth(a, origin, vec3(0.0f, 1.0f, 0.0f), 32);
    const vec3 grazing =
        AtmosphereOpticalDepth(a, origin, glm::normalize(vec3(1.0f, 0.02f, 0.0f)), 32);
    CHECK(grazing.r > up.r);
    CHECK(grazing.g > up.g);
    CHECK(grazing.b > up.b);
}
