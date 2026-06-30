#pragma once

#include <array>

#include <Veng/Veng.h>

namespace Veng
{
    /// @brief Order-2 spherical-harmonics coefficient set — 9 RGB coefficients.
    ///
    /// glm-only value type (copied freely like AABB/vec3, no ownership rule), so it stays
    /// inside the public/backend include-hygiene split. Holds the nine order-2 real-SH
    /// coefficients of a band-limited spherical function, one vec3 per coefficient so a
    /// color channel projects, convolves, and evaluates together. Index `i` of `Coefficients`
    /// pairs with the `i`-th basis function `ShBasis` returns, so a `Sh9` serializes into a
    /// constants buffer as nine consecutive vec4s (the .w padding the GPU std140 layout adds).
    ///
    /// The convention is the real SH basis with constant per-direction coefficients
    /// (Ramamoorthi & Hanrahan / Sloan): a `ProjectSample` accumulates radiance, a single
    /// `ConvolveCosine` turns that radiance set into an irradiance set by the analytic
    /// Lambertian cosine-lobe band factors, and `EvalIrradiance` reconstructs the
    /// cosine-weighted hemisphere integral in a normal direction.
    struct Sh9
    {
        /// @brief The nine order-2 coefficients, one RGB triple each, indexed to match ShBasis.
        std::array<vec3, 9> Coefficients;

        /// @brief Returns the all-zero coefficient set, the additive identity for projection.
        ///
        /// Folding zero samples yields Zero(), and ProjectSample accumulates onto it, so a
        /// freshly-zeroed set plus N samples is the sum of the N per-sample projections.
        [[nodiscard]] static Sh9 Zero()
        {
            Sh9 result{};
            for (vec3& coefficient : result.Coefficients)
            {
                coefficient = vec3(0.0f);
            }
            return result;
        }
    };

    /// @brief Number of order-2 real-SH bands (l = 0, 1, 2).
    inline constexpr u32 ShBands = 3;

    /// @brief Number of order-2 real-SH coefficients across all bands (1 + 3 + 5).
    inline constexpr u32 ShCoefficientCount = 9;

    /// @brief Evaluates the nine order-2 real-SH basis functions in a direction.
    ///
    /// The basis is the standard real SH with constant coefficients: index 0 is the constant
    /// band l=0; indices 1–3 are the linear band l=1 (in y, z, x order); indices 4–8 are the
    /// quadratic band l=2. ProjectSample, ConvolveCosine, and EvalIrradiance all share this
    /// one basis, so a consumer (a GPU evaluator) reproducing these exact polynomials matches
    /// the CPU bit for bit.
    /// @param direction  The direction to evaluate the basis in. Assumed unit length; a
    ///                    non-normalized direction scales the higher bands and is caller error.
    /// @return The nine basis-function values, indexed to match Sh9::Coefficients.
    [[nodiscard]] inline std::array<f32, 9> ShBasis(vec3 direction)
    {
        const f32 x = direction.x;
        const f32 y = direction.y;
        const f32 z = direction.z;

        return std::array<f32, 9>{
            0.282095f,                         // l=0
            0.488603f * y,                     // l=1, m=-1
            0.488603f * z,                     // l=1, m=0
            0.488603f * x,                     // l=1, m=+1
            1.092548f * x * y,                 // l=2, m=-2
            1.092548f * y * z,                 // l=2, m=-1
            0.315392f * (3.0f * z * z - 1.0f), // l=2, m=0
            1.092548f * x * z,                 // l=2, m=+1
            0.546274f * (x * x - y * y),       // l=2, m=+2
        };
    }

    /// @brief Accumulates a single (direction, radiance) sample into an SH set.
    ///
    /// Adds `radiance * ShBasis(direction)[i] * weight` to coefficient `i`. The weight is the
    /// per-sample solid-angle measure: a uniform Monte-Carlo projection over the whole sphere
    /// uses `4*pi / sampleCount` so the coefficients integrate rather than average, and an
    /// analytic sample carries its own measure. Repeated calls onto one set sum linearly, so
    /// projecting a sum of radiances equals the sum of the per-radiance projections.
    /// @param sh         The set to accumulate into, in place.
    /// @param direction  The sample direction. Assumed unit length.
    /// @param radiance   The radiance arriving from `direction`, per color channel.
    /// @param weight     The sample's solid-angle weight (4*pi / N for a uniform sphere MC).
    inline void ProjectSample(Sh9& sh, vec3 direction, vec3 radiance, f32 weight)
    {
        const std::array<f32, 9> basis = ShBasis(direction);
        for (u32 i = 0; i < ShCoefficientCount; ++i)
        {
            sh.Coefficients[i] += radiance * (basis[i] * weight);
        }
    }

    /// @brief Convolves a radiance SH set with the Lambertian cosine lobe, yielding irradiance.
    ///
    /// Multiplies each band by its analytic cosine-lobe transfer factor — A0 = pi for the
    /// l=0 band, A1 = 2*pi/3 for l=1, A2 = pi/4 for l=2 — the closed-form result of convolving
    /// the clamped-cosine kernel into SH. A single EvalIrradiance on the convolved set is then
    /// the cosine-weighted hemisphere integral (the diffuse response), so the per-fragment cost
    /// is one nine-term dot product rather than a hemisphere integration.
    /// @param radiance  A radiance SH set (from ProjectSample over an environment).
    /// @return The irradiance SH set: the same band layout, cosine-convolved.
    [[nodiscard]] inline Sh9 ConvolveCosine(const Sh9& radiance)
    {
        constexpr f32 Pi = 3.14159265358979323846f;
        constexpr f32 A0 = Pi;
        constexpr f32 A1 = (2.0f * Pi) / 3.0f;
        constexpr f32 A2 = Pi / 4.0f;
        constexpr std::array<f32, 9> BandFactor{A0, A1, A1, A1, A2, A2, A2, A2, A2};

        Sh9 result{};
        for (u32 i = 0; i < ShCoefficientCount; ++i)
        {
            result.Coefficients[i] = radiance.Coefficients[i] * BandFactor[i];
        }
        return result;
    }

    /// @brief Reconstructs irradiance from a cosine-convolved SH set in a normal direction.
    ///
    /// Evaluates `sum(irradiance[i] * ShBasis(normal)[i])` — the SH dot product. The set must
    /// already be cosine-convolved (the output of ConvolveCosine); evaluating a raw radiance
    /// set instead reconstructs radiance, not irradiance. A constant radiance L projected over
    /// the sphere, convolved, and evaluated reproduces the cosine-weighted integral pi*L,
    /// independent of the normal, with the higher bands contributing zero.
    /// @param irradiance  A cosine-convolved SH set (the output of ConvolveCosine).
    /// @param normal      The surface normal to reconstruct irradiance for. Assumed unit length.
    /// @return The reconstructed irradiance, per color channel. May be negative for a set whose
    ///         higher bands ring below zero; a consumer clamps to non-negative where required.
    [[nodiscard]] inline vec3 EvalIrradiance(const Sh9& irradiance, vec3 normal)
    {
        const std::array<f32, 9> basis = ShBasis(normal);
        vec3 result(0.0f);
        for (u32 i = 0; i < ShCoefficientCount; ++i)
        {
            result += irradiance.Coefficients[i] * basis[i];
        }
        return result;
    }
}
