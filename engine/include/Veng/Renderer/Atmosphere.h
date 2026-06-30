#pragma once

#include <Veng/Veng.h>
#include <Veng/Reflection/Reflect.h>

#include <cmath>

/// @brief Bruneton precomputed-atmospheric-scattering parameters and device-free sky math.
///
/// The Atmosphere struct is a physically-based description of a planet's air shell —
/// Rayleigh / Mie / ozone extinction, the planet + atmosphere radii, and the sun's
/// angular size and irradiance. Its lookup tables (transmittance, scattering, irradiance)
/// are precomputed once on the GPU; at runtime the sky for any sun direction is a cheap
/// table sample, so a day/night cycle costs nothing per frame.
///
/// This header additionally carries a self-contained CPU reference evaluation of the same
/// model (single-scattering, ray-marched) so the parameterization is testable without a
/// device and so an ambient term can read a plausible sky radiance off the render thread.
/// The CPU path is glm-only and allocates nothing.
namespace Veng::Renderer
{
    /// @brief Physically-based parameters of a planetary atmosphere.
    ///
    /// Lengths are in kilometers and scattering coefficients in inverse kilometers, the
    /// scale Bruneton's tables are conventionally built in. The defaults describe Earth at
    /// sea level. The struct is reflected, so the editor inspects and edits it for free.
    struct Atmosphere
    {
        /// @brief Rayleigh scattering coefficient at sea level, per RGB channel (1/km).
        ///
        /// Air's wavelength-dependent (blue-biased) molecular scattering — the reason a
        /// clear daytime sky is blue and a low sun reddens.
        vec3 RayleighScattering{5.802e-3f, 13.558e-3f, 33.1e-3f};

        /// @brief Exponential height the Rayleigh density falls off over (km).
        f32 RayleighHeight = 8.0f;

        /// @brief Mie scattering coefficient at sea level (1/km), grey across RGB.
        ///
        /// Aerosol/haze scattering — the forward-biased glow around the sun and the white
        /// horizon band.
        vec3 MieScattering{3.996e-3f, 3.996e-3f, 3.996e-3f};

        /// @brief Mie extinction coefficient at sea level (1/km); scattering plus absorption.
        f32 MieExtinction = 4.44e-3f;

        /// @brief Exponential height the Mie density falls off over (km).
        f32 MieHeight = 1.2f;

        /// @brief Henyey-Greenstein anisotropy of Mie scattering, in (-1, 1).
        ///
        /// Positive is forward-scattering (the bright sun halo); 0.8 is the standard haze value.
        f32 MieAnisotropy = 0.8f;

        /// @brief Ozone absorption coefficient at the layer peak, per RGB channel (1/km).
        ///
        /// The ozone layer absorbs in the green/red, deepening the blue of a high sky and the
        /// twilight band. Ozone scatters negligibly; this is pure extinction.
        vec3 OzoneAbsorption{0.650e-3f, 1.881e-3f, 0.085e-3f};

        /// @brief Center altitude of the ozone layer above the surface (km).
        f32 OzoneCenter = 25.0f;

        /// @brief Half-width of the (tent-shaped) ozone layer (km).
        f32 OzoneWidth = 15.0f;

        /// @brief Planet (ground) radius (km).
        f32 PlanetRadius = 6360.0f;

        /// @brief Top-of-atmosphere radius (km); the shell the sky integrates within.
        f32 AtmosphereRadius = 6420.0f;

        /// @brief Angular radius of the sun disk (radians); ~0.00465 for Earth's sun.
        f32 SunAngularRadius = 0.004675f;

        /// @brief Extra-terrestrial solar irradiance scale (unitless, per RGB).
        ///
        /// Multiplies the scattered sky radiance and drives the sun-disk brightness; the
        /// default is a balanced white the tonemap maps to a daylit sky.
        vec3 SunIrradiance{1.0f, 1.0f, 1.0f};
    };

    /// @brief Solves the near intersection of a ray with a sphere centered at the origin.
    /// @param origin    Ray origin (km), relative to the planet center.
    /// @param dir       Normalized ray direction.
    /// @param radius    Sphere radius (km).
    /// @return The positive distance to the far intersection, or 0 if the ray misses.
    [[nodiscard]] inline f32 AtmosphereRaySphere(const vec3& origin, const vec3& dir, f32 radius)
    {
        const f32 b = glm::dot(origin, dir);
        const f32 c = glm::dot(origin, origin) - radius * radius;
        const f32 disc = b * b - c;
        if (disc < 0.0f)
        {
            return 0.0f;
        }
        const f32 sqrtDisc = std::sqrt(disc);
        const f32 far = -b + sqrtDisc;
        return far > 0.0f ? far : 0.0f;
    }

    /// @brief Rayleigh phase function for a scattering angle cosine.
    /// @param cosTheta  Cosine of the angle between view and light directions.
    /// @return The normalized phase value.
    [[nodiscard]] inline f32 AtmosphereRayleighPhase(f32 cosTheta)
    {
        return 3.0f / (16.0f * glm::pi<f32>()) * (1.0f + cosTheta * cosTheta);
    }

    /// @brief Henyey-Greenstein Mie phase function for a scattering angle cosine.
    /// @param cosTheta  Cosine of the angle between view and light directions.
    /// @param g         Anisotropy in (-1, 1).
    /// @return The normalized phase value.
    [[nodiscard]] inline f32 AtmosphereMiePhase(f32 cosTheta, f32 g)
    {
        const f32 g2 = g * g;
        const f32 denom = 1.0f + g2 - 2.0f * g * cosTheta;
        return (1.0f - g2) / (4.0f * glm::pi<f32>() * std::pow(std::max(denom, 1e-4f), 1.5f));
    }

    /// @brief Atmospheric densities at an altitude above the surface.
    struct AtmosphereDensity
    {
        /// @brief Rayleigh density (relative to sea level).
        f32 Rayleigh = 0.0f;
        /// @brief Mie density (relative to sea level).
        f32 Mie = 0.0f;
        /// @brief Ozone density (relative to the layer peak), tent-shaped about the center.
        f32 Ozone = 0.0f;
    };

    /// @brief Evaluates the three density profiles at an altitude.
    /// @param atmosphere  The atmosphere parameters.
    /// @param altitude    Height above the surface (km); negative is clamped to 0.
    /// @return The Rayleigh / Mie / ozone densities at that altitude.
    [[nodiscard]] inline AtmosphereDensity AtmosphereDensities(const Atmosphere& atmosphere,
                                                               f32 altitude)
    {
        const f32 h = std::max(altitude, 0.0f);
        AtmosphereDensity d;
        d.Rayleigh = std::exp(-h / atmosphere.RayleighHeight);
        d.Mie = std::exp(-h / atmosphere.MieHeight);
        // Tent profile: 1 at the layer center, falling linearly to 0 at +/- the half-width.
        d.Ozone =
            std::max(0.0f, 1.0f - std::abs(h - atmosphere.OzoneCenter) / atmosphere.OzoneWidth);
        return d;
    }

    /// @brief Combined per-channel extinction at one density sample (Rayleigh + Mie + ozone).
    /// @param atmosphere  The atmosphere parameters.
    /// @param d           The densities at the sample point.
    /// @return The per-channel extinction coefficient (1/km).
    [[nodiscard]] inline vec3 AtmosphereSegmentExtinction(const Atmosphere& atmosphere,
                                                          const AtmosphereDensity& d)
    {
        const f32 mieRatio = atmosphere.MieExtinction / std::max(atmosphere.MieScattering.x, 1e-9f);
        return atmosphere.RayleighScattering * d.Rayleigh +
               atmosphere.MieScattering * (mieRatio * d.Mie) + atmosphere.OzoneAbsorption * d.Ozone;
    }

    /// @brief Optical depth along a ray segment from a point toward the top of atmosphere.
    ///
    /// Ray-marches the combined Rayleigh + Mie + ozone extinction; the per-channel
    /// transmittance is exp(-result). A short march (a few dozen steps) is enough for the
    /// smooth exponential profiles.
    /// @param atmosphere  The atmosphere parameters.
    /// @param origin      March start, relative to the planet center (km).
    /// @param dir         Normalized march direction.
    /// @param steps       Number of integration steps.
    /// @return The per-channel optical depth (extinction integral), RGB.
    [[nodiscard]] inline vec3 AtmosphereOpticalDepth(const Atmosphere& atmosphere,
                                                     const vec3& origin, const vec3& dir, u32 steps)
    {
        const f32 march = AtmosphereRaySphere(origin, dir, atmosphere.AtmosphereRadius);
        if (march <= 0.0f)
        {
            return vec3(0.0f);
        }
        const f32 dt = march / static_cast<f32>(steps);
        vec3 depth(0.0f);
        for (u32 i = 0; i < steps; ++i)
        {
            const vec3 p = origin + dir * ((static_cast<f32>(i) + 0.5f) * dt);
            const f32 altitude = glm::length(p) - atmosphere.PlanetRadius;
            const AtmosphereDensity d = AtmosphereDensities(atmosphere, altitude);
            depth += AtmosphereSegmentExtinction(atmosphere, d) * dt;
        }
        return depth;
    }

    /// @brief Single-scattering sky radiance along a view ray, ray-marched on the CPU.
    ///
    /// A device-free reference of the same model the GPU tables precompute: it marches the
    /// view ray through the shell, accumulating in-scattered sunlight attenuated by the
    /// transmittance to the viewer and to the sun. Single-scattering only (the GPU path adds
    /// multiple scattering), but it reproduces the day/night and horizon behavior the tables
    /// must — noon is brighter and bluer, the horizon warms, and a sun below the horizon
    /// gives near-black. The result is linear HDR radiance, pre-tonemap.
    /// @param atmosphere    The atmosphere parameters.
    /// @param viewDir       Normalized view direction (world up is +Y).
    /// @param sunDir        Normalized direction toward the sun (world up is +Y).
    /// @param viewAltitude  Viewer altitude above the surface (km).
    /// @param steps         View-ray integration steps.
    /// @return The linear RGB sky radiance for that view direction.
    [[nodiscard]] inline vec3 AtmosphereSkyRadiance(const Atmosphere& atmosphere,
                                                    const vec3& viewDir, const vec3& sunDir,
                                                    f32 viewAltitude = 0.2f, u32 steps = 32)
    {
        const vec3 origin(0.0f, atmosphere.PlanetRadius + std::max(viewAltitude, 0.0f), 0.0f);
        const vec3 view = glm::normalize(viewDir);
        const vec3 sun = glm::normalize(sunDir);

        const f32 march = AtmosphereRaySphere(origin, view, atmosphere.AtmosphereRadius);
        if (march <= 0.0f)
        {
            return vec3(0.0f);
        }

        const f32 cosTheta = glm::dot(view, sun);
        const f32 rayleighPhase = AtmosphereRayleighPhase(cosTheta);
        const f32 miePhase = AtmosphereMiePhase(cosTheta, atmosphere.MieAnisotropy);
        const vec3 mieScattering = atmosphere.MieScattering;

        const f32 dt = march / static_cast<f32>(steps);
        vec3 radiance(0.0f);
        vec3 opticalDepthToViewer(0.0f);
        for (u32 i = 0; i < steps; ++i)
        {
            const vec3 p = origin + view * ((static_cast<f32>(i) + 0.5f) * dt);
            const f32 altitude = glm::length(p) - atmosphere.PlanetRadius;
            const AtmosphereDensity d = AtmosphereDensities(atmosphere, altitude);

            opticalDepthToViewer += AtmosphereSegmentExtinction(atmosphere, d) * dt;

            // Sun must clear the planet for this sample to be lit.
            const f32 sunGround = AtmosphereRaySphere(p, sun, atmosphere.PlanetRadius);
            if (sunGround > 0.0f)
            {
                continue;
            }
            const vec3 opticalDepthToSun = AtmosphereOpticalDepth(atmosphere, p, sun, 16);
            const vec3 transmittance = glm::exp(-(opticalDepthToViewer + opticalDepthToSun));

            const vec3 inscatter = (atmosphere.RayleighScattering * d.Rayleigh * rayleighPhase +
                                    mieScattering * d.Mie * miePhase);
            radiance += transmittance * inscatter * dt;
        }
        return radiance * atmosphere.SunIrradiance;
    }
}

VE_REFLECT(::Veng::Renderer::Atmosphere, 0x5363D2B4FB9F949BULL)
VE_FIELD(RayleighScattering, .DisplayName = "Rayleigh scattering")
VE_FIELD(RayleighHeight, .DisplayName = "Rayleigh height", .Display = {.Min = 0.001})
VE_FIELD(MieScattering, .DisplayName = "Mie scattering")
VE_FIELD(MieExtinction, .DisplayName = "Mie extinction", .Display = {.Min = 0.0})
VE_FIELD(MieHeight, .DisplayName = "Mie height", .Display = {.Min = 0.001})
VE_FIELD(MieAnisotropy, .DisplayName = "Mie anisotropy", .Display = {.Min = -0.99, .Max = 0.99})
VE_FIELD(OzoneAbsorption, .DisplayName = "Ozone absorption")
VE_FIELD(OzoneCenter, .DisplayName = "Ozone center", .Display = {.Min = 0.0})
VE_FIELD(OzoneWidth, .DisplayName = "Ozone width", .Display = {.Min = 0.001})
VE_FIELD(PlanetRadius, .DisplayName = "Planet radius", .Display = {.Min = 1.0})
VE_FIELD(AtmosphereRadius, .DisplayName = "Atmosphere radius", .Display = {.Min = 1.0})
VE_FIELD(SunAngularRadius, .DisplayName = "Sun angular radius", .Display = {.Min = 0.0})
VE_FIELD(SunIrradiance, .DisplayName = "Sun irradiance")
VE_REFLECT_END();
