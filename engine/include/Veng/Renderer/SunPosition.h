#pragma once

#include <Veng/Veng.h>
#include <Veng/Reflection/Reflect.h>

#include <cmath>

/// @brief Time-of-day sun positioning: orbital parameters and device-free solar math.
///
/// Derives a toward-sun direction from a clock time instead of an authored vector, so a
/// day/night cycle is driven by one scalar. The model is the standard solar-position
/// derivation for a circular orbit: the seasonal declination from the axial tilt and the
/// day of year, the hour angle from the time of day, and the horizon-frame elevation and
/// azimuth from the site latitude. glm-only, allocation-free, and testable without a
/// device, like the Atmosphere CPU reference beside it.
namespace Veng::Renderer
{
    /// @brief Orbital and site parameters the sun direction is derived from.
    ///
    /// Angles are in degrees (the authoring-friendly unit) and the year length in days.
    /// The defaults describe Earth at a temperate northern latitude. The struct is
    /// reflected, so the editor inspects and edits it for free.
    struct SunOrbit
    {
        /// @brief Axial tilt of the planet's spin axis against its orbital plane (degrees).
        ///
        /// Drives the seasonal declination swing: the sun's noon elevation varies by
        /// +/- this angle over the year. 0 gives identical days year-round.
        f32 AxialTilt = 23.44f;

        /// @brief Site latitude (degrees); positive north of the equator.
        f32 Latitude = 45.0f;

        /// @brief Length of the planet's year (days); the period of the seasonal cycle.
        f32 YearLength = 365.25f;

        /// @brief Compass heading of world north (degrees).
        ///
        /// 0 points north along -Z (east +X); positive rotates north from -Z toward +X
        /// about +Y. Spins the sun's path without rotating the world.
        f32 NorthHeading = 0.0f;
    };

    /// @brief Solar declination for a day of the year (radians).
    ///
    /// Exact for a circular orbit: asin(sin(tilt) * sin(yearPhase)). Day 0 is the northern
    /// spring equinox (declination 0, rising), the northern summer solstice at a quarter
    /// year; the day wraps over the year length.
    /// @param orbit      The orbital parameters.
    /// @param dayOfYear  Day of the year, measured from the northern spring equinox.
    /// @return The declination in radians, in [-tilt, +tilt].
    [[nodiscard]] inline f32 ComputeSunDeclination(const SunOrbit& orbit, f32 dayOfYear)
    {
        const f32 year = std::max(orbit.YearLength, 1e-3f);
        const f32 phase = glm::two_pi<f32>() * (dayOfYear / year);
        return std::asin(std::sin(glm::radians(orbit.AxialTilt)) * std::sin(phase));
    }

    /// @brief Normalized toward-sun direction for a time of day (world up +Y).
    ///
    /// Solar time: 12 is solar noon (the sun's daily maximum, toward the equator), 0/24
    /// solar midnight, 6 sunrise-side east and 18 sunset-side west at an equinox. Hours
    /// beyond [0, 24) wrap. A negative Y means the sun is below the horizon; the sky and
    /// lighting consume the direction as-is, so night falls out of the same math.
    /// @param orbit      The orbital parameters.
    /// @param hours      Time of day in solar hours; noon is 12.
    /// @param dayOfYear  Day of the year, measured from the northern spring equinox.
    /// @return The unit toward-sun direction.
    [[nodiscard]] inline vec3 ComputeSunDirection(const SunOrbit& orbit, f32 hours,
                                                  f32 dayOfYear = 0.0f)
    {
        const f32 declination = ComputeSunDeclination(orbit, dayOfYear);
        const f32 hourAngle = glm::two_pi<f32>() * (hours / 24.0f - 0.5f);
        const f32 latitude = glm::radians(orbit.Latitude);

        // Horizon-frame (east/north/up) sun vector from the spherical-astronomy identities.
        const f32 sinDec = std::sin(declination);
        const f32 cosDec = std::cos(declination);
        const f32 sinLat = std::sin(latitude);
        const f32 cosLat = std::cos(latitude);
        const f32 east = -cosDec * std::sin(hourAngle);
        const f32 north = cosLat * sinDec - sinLat * cosDec * std::cos(hourAngle);
        const f32 up = sinLat * sinDec + cosLat * cosDec * std::cos(hourAngle);

        // Map the horizon frame into the world: up is +Y, north is -Z yawed by NorthHeading.
        const f32 heading = glm::radians(orbit.NorthHeading);
        const vec3 northAxis(std::sin(heading), 0.0f, -std::cos(heading));
        const vec3 eastAxis(std::cos(heading), 0.0f, std::sin(heading));
        return glm::normalize(eastAxis * east + northAxis * north + vec3(0.0f, up, 0.0f));
    }
}

VE_REFLECT(::Veng::Renderer::SunOrbit, 0x9C5F53431BCB3753ULL)
VE_FIELD(AxialTilt, .DisplayName = "Axial tilt", .Tooltip = "Degrees; the seasonal swing",
         .Display = {.Min = 0.0, .Max = 180.0})
VE_FIELD(Latitude, .DisplayName = "Latitude", .Tooltip = "Degrees; positive is north",
         .Display = {.Min = -90.0, .Max = 90.0})
VE_FIELD(YearLength, .DisplayName = "Year length", .Tooltip = "Days per year",
         .Display = {.Min = 0.001})
VE_FIELD(NorthHeading, .DisplayName = "North heading", .Tooltip = "Degrees; 0 is north along -Z",
         .Display = {.Min = -180.0, .Max = 180.0})
VE_REFLECT_END();
