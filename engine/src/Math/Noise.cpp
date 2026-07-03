#include <Veng/Math/Noise.h>

#include <array>
#include <cmath>

namespace Veng
{
    namespace
    {
        // Folds an integer lattice coordinate into the hash path: floor(p) as
        // signed integers, reinterpreted as u64 lanes, combined with the seed.
        u64 LatticeHash(i32 x, i32 y, i32 z, u64 seed)
        {
            const std::array<u64, 3> coords = {
                static_cast<u64>(static_cast<u32>(x)),
                static_cast<u64>(static_cast<u32>(y)),
                static_cast<u64>(static_cast<u32>(z)),
            };
            return HashCombine(Hash64(std::span<const u64>(coords)), seed);
        }

        // Maps a 64-bit hash to a value in [-1, 1].
        f32 HashToSigned(u64 hash)
        {
            return static_cast<f32>(hash >> 40) * (1.0f / 8388608.0f) - 1.0f;
        }

        // Maps a 64-bit hash to a pseudo-random unit gradient direction.
        vec3 HashToGradient(u64 hash)
        {
            const f32 theta = HashToSigned(hash) * glm::pi<f32>();
            const f32 z = HashToSigned(Hash64(hash));
            const f32 r = glm::sqrt(glm::max(0.0f, 1.0f - z * z));
            return vec3(r * std::cos(theta), r * std::sin(theta), z);
        }

        // Hermite smoothstep, the interpolant both noise kinds share.
        f32 Smooth(f32 t)
        {
            return t * t * t * (t * (t * 6.0f - 15.0f) + 10.0f);
        }

        struct Lattice
        {
            ivec3 Cell;
            vec3 Fraction;
        };

        Lattice Floor(vec3 p)
        {
            return Lattice{
                .Cell = ivec3(static_cast<i32>(std::floor(p.x)), static_cast<i32>(std::floor(p.y)),
                              static_cast<i32>(std::floor(p.z))),
                .Fraction =
                    vec3(p.x - std::floor(p.x), p.y - std::floor(p.y), p.z - std::floor(p.z)),
            };
        }
    }

    f32 ValueNoise(vec3 p, u64 seed)
    {
        const Lattice lattice = Floor(p);
        const vec3 t(Smooth(lattice.Fraction.x), Smooth(lattice.Fraction.y),
                     Smooth(lattice.Fraction.z));

        f32 corners[2][2][2];
        for (i32 dz = 0; dz < 2; ++dz)
        {
            for (i32 dy = 0; dy < 2; ++dy)
            {
                for (i32 dx = 0; dx < 2; ++dx)
                {
                    const u64 hash = LatticeHash(lattice.Cell.x + dx, lattice.Cell.y + dy,
                                                 lattice.Cell.z + dz, seed);
                    corners[dx][dy][dz] = HashToSigned(hash);
                }
            }
        }

        const f32 x00 = glm::mix(corners[0][0][0], corners[1][0][0], t.x);
        const f32 x10 = glm::mix(corners[0][1][0], corners[1][1][0], t.x);
        const f32 x01 = glm::mix(corners[0][0][1], corners[1][0][1], t.x);
        const f32 x11 = glm::mix(corners[0][1][1], corners[1][1][1], t.x);
        const f32 y0 = glm::mix(x00, x10, t.y);
        const f32 y1 = glm::mix(x01, x11, t.y);
        return glm::mix(y0, y1, t.z);
    }

    f32 GradientNoise(vec3 p, u64 seed)
    {
        const Lattice lattice = Floor(p);
        const vec3 t(Smooth(lattice.Fraction.x), Smooth(lattice.Fraction.y),
                     Smooth(lattice.Fraction.z));

        f32 corners[2][2][2];
        for (i32 dz = 0; dz < 2; ++dz)
        {
            for (i32 dy = 0; dy < 2; ++dy)
            {
                for (i32 dx = 0; dx < 2; ++dx)
                {
                    const u64 hash = LatticeHash(lattice.Cell.x + dx, lattice.Cell.y + dy,
                                                 lattice.Cell.z + dz, seed);
                    const vec3 gradient = HashToGradient(hash);
                    const vec3 offset =
                        lattice.Fraction -
                        vec3(static_cast<f32>(dx), static_cast<f32>(dy), static_cast<f32>(dz));
                    corners[dx][dy][dz] = glm::dot(gradient, offset);
                }
            }
        }

        const f32 x00 = glm::mix(corners[0][0][0], corners[1][0][0], t.x);
        const f32 x10 = glm::mix(corners[0][1][0], corners[1][1][0], t.x);
        const f32 x01 = glm::mix(corners[0][0][1], corners[1][0][1], t.x);
        const f32 x11 = glm::mix(corners[0][1][1], corners[1][1][1], t.x);
        const f32 y0 = glm::mix(x00, x10, t.y);
        const f32 y1 = glm::mix(x01, x11, t.y);
        // Gradient noise's raw range is +-0.5*sqrt(3) at the lattice diagonal;
        // rescale so the common case saturates close to +-1 like ValueNoise.
        return glm::mix(y0, y1, t.z) * 1.5f;
    }

    f32 Fbm(vec3 p, u64 seed, u32 octaves, f32 lacunarity, f32 gain)
    {
        f32 sum = 0.0f;
        f32 amplitude = 1.0f;
        f32 amplitudeSum = 0.0f;
        vec3 frequency = p;
        for (u32 octave = 0; octave < octaves; ++octave)
        {
            sum += GradientNoise(frequency, HashCombine(seed, octave)) * amplitude;
            amplitudeSum += amplitude;
            amplitude *= gain;
            frequency *= lacunarity;
        }
        return amplitudeSum > 0.0f ? sum / amplitudeSum : 0.0f;
    }
}
