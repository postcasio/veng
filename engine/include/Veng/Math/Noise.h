#pragma once

#include <Veng/Math/Random.h>
#include <Veng/Veng.h>

/// @brief Coherent value/gradient noise and fractal Brownian motion — pure,
/// device-free, allocation-free.
///
/// Built over `Hash64` (see `Veng/Math/Random.h`): every lattice corner's random
/// contribution is a pure function of its integer coordinate and the seed, so the
/// field needs no permutation table or precomputed gradient set. Existence-affecting
/// decisions must ride the integer hash path — this header supplies cosmetic
/// magnitude and variation (density perturbation, color/appearance variation), never
/// which objects exist, since float evaluation is not asserted bit-identical across
/// platforms the way the integer hashes are.
namespace Veng
{
    /// @brief Value noise (interpolated random lattice values) in 3D, seedable.
    ///
    /// Smoothstep-interpolates a hashed random value at each of the 8 surrounding
    /// integer lattice corners. Continuous (C1) everywhere, range approximately
    /// [-1, 1].
    /// @param p     Sample position.
    /// @param seed  Seed distinguishing independent noise fields.
    /// @return The noise value, approximately in [-1, 1].
    [[nodiscard]] f32 ValueNoise(vec3 p, u64 seed);

    /// @brief Gradient (Perlin-style) noise in 3D, seedable.
    ///
    /// Dot-products a pseudo-random unit gradient at each of the 8 surrounding
    /// integer lattice corners against the offset to the sample point, then
    /// smoothstep-interpolates. Continuous (C1) everywhere, range approximately
    /// [-1, 1].
    /// @param p     Sample position.
    /// @param seed  Seed distinguishing independent noise fields.
    /// @return The noise value, approximately in [-1, 1].
    [[nodiscard]] f32 GradientNoise(vec3 p, u64 seed);

    /// @brief Fractal Brownian motion: a weighted sum of octaves of `GradientNoise`.
    ///
    /// Each octave samples at `frequency *= lacunarity` and contributes at
    /// `amplitude *= gain`, so higher octaves add finer, weaker detail. The result
    /// is normalized by the summed amplitude, keeping the range approximately
    /// [-1, 1] regardless of octave count.
    /// @param p           Sample position.
    /// @param seed        Seed distinguishing independent noise fields.
    /// @param octaves     Number of octaves to sum (1 reduces to a single
    ///                    `GradientNoise` evaluation).
    /// @param lacunarity  Frequency multiplier per octave.
    /// @param gain        Amplitude multiplier per octave.
    /// @return The summed, normalized noise value, approximately in [-1, 1].
    [[nodiscard]] f32 Fbm(vec3 p, u64 seed, u32 octaves, f32 lacunarity = 2.0f, f32 gain = 0.5f);
}
