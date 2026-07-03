#pragma once

#include <span>

#include <Veng/Veng.h>

/// @brief Deterministic hashing and a seedable PRNG stream — pure, device-free,
/// allocation-free.
///
/// Any procedural feature needing reproducible pseudo-randomness derives it from a
/// hash of its own address: `Hash64`/`HashCombine` mix integers (SplitMix64-family),
/// and `Rng` is a seedable PCG stream a caller addresses as `Rng(HashCombine(seed,
/// address))` for an independent, reproducible per-object stream. Same inputs yield
/// the same outputs on every platform — the integer paths are bit-exact, with no
/// floating-point nondeterminism. Not cryptographic: this is speed and distribution
/// quality, not security.
namespace Veng
{
    /// @brief A stateless 64-bit avalanche mix (the SplitMix64 finalizer).
    ///
    /// Pure and bijective; every input bit influences every output bit. Used both to
    /// finalize a hash and, seeded with an address, to seed an independent `Rng`
    /// stream. `constexpr` so a compile-time-known address hashes at compile time.
    /// @param x  The value to mix.
    /// @return The avalanched 64-bit hash.
    [[nodiscard]] constexpr u64 Hash64(u64 x)
    {
        x ^= x >> 30;
        x *= 0xBF58476D1CE4E5B9ULL;
        x ^= x >> 27;
        x *= 0x94D049BB133111EBULL;
        x ^= x >> 31;
        return x;
    }

    /// @brief Combines a running hash with another value, order-dependent.
    ///
    /// Folds `value` into `seed` (via addition and the golden-ratio constant, in the
    /// boost::hash_combine mold) then re-avalanches, so `HashCombine(a, b) !=
    /// HashCombine(b, a)` in general — the accumulation order of a multi-field key is
    /// part of its identity.
    /// @param seed   The running hash to fold into.
    /// @param value  The value to combine.
    /// @return The combined, re-avalanched hash.
    [[nodiscard]] constexpr u64 HashCombine(u64 seed, u64 value)
    {
        return Hash64(seed ^ (value + 0x9E3779B97F4A7C15ULL + (seed << 6) + (seed >> 2)));
    }

    /// @brief Hashes a small tuple of integers into one 64-bit value.
    ///
    /// Folds each element into a running hash via `HashCombine`, in order — the
    /// natural hash for a spatial or hierarchical address (a cell coordinate, an
    /// index path) expressed as a span of integers.
    /// @param values  The integers to hash, in order.
    /// @return The combined hash of the whole span.
    [[nodiscard]] constexpr u64 Hash64(std::span<const u64> values)
    {
        u64 seed = 0;
        for (const u64 value : values)
        {
            seed = HashCombine(seed, value);
        }
        return seed;
    }

    /// @brief A small, fast, seedable PRNG stream (PCG32 XSH-RR).
    ///
    /// A value type: copying an `Rng` copies its state, so the copy is an independent
    /// stream from the same point — there is no shared or global RNG. Seed it from a
    /// hashed address (`Rng(HashCombine(seed, address))`) for a reproducible,
    /// independent stream per object, addressable with no table. Not cryptographic.
    struct Rng
    {
        /// @brief Seeds a new stream.
        /// @param seed  The 64-bit seed; any two distinct seeds diverge immediately.
        explicit Rng(u64 seed) : m_State(0), m_Increment((seed << 1) | 1)
        {
            Step();
            m_State += seed;
            Step();
        }

        /// @brief Returns the next pseudo-random 32-bit value in the stream.
        [[nodiscard]] u32 NextU32()
        {
            const u64 previous = m_State;
            Step();
            const u32 xorshifted = static_cast<u32>(((previous >> 18u) ^ previous) >> 27u);
            const u32 rotation = static_cast<u32>(previous >> 59u);
            return (xorshifted >> rotation) | (xorshifted << ((~rotation + 1u) & 31u));
        }

        /// @brief Returns the next pseudo-random 64-bit value, from two 32-bit draws.
        [[nodiscard]] u64 NextU64()
        {
            const u64 high = NextU32();
            const u64 low = NextU32();
            return (high << 32) | low;
        }

        /// @brief Returns the next pseudo-random float in [0, 1).
        [[nodiscard]] f32 NextFloat()
        {
            // 24 bits of mantissa precision, the usual [0,1) float-from-uint recipe:
            // the top 24 bits of the draw scaled by 2^-24.
            return static_cast<f32>(NextU32() >> 8) * (1.0f / 16777216.0f);
        }

        /// @brief Returns the next pseudo-random float in [lo, hi).
        /// @param lo  Inclusive lower bound.
        /// @param hi  Exclusive upper bound.
        [[nodiscard]] f32 NextFloat(f32 lo, f32 hi) { return lo + NextFloat() * (hi - lo); }

        /// @brief Returns a point sampled uniformly from the unit ball (|p| <= 1).
        ///
        /// Rejection-samples the unit cube — a small, unbounded but fast-converging
        /// loop, appropriate for a CPU-side generation helper.
        [[nodiscard]] vec3 NextInSphere()
        {
            vec3 p;
            do
            {
                p = vec3(NextFloat(-1.0f, 1.0f), NextFloat(-1.0f, 1.0f), NextFloat(-1.0f, 1.0f));
            } while (glm::dot(p, p) > 1.0f);
            return p;
        }

    private:
        void Step() { m_State = m_State * 6364136223846793005ULL + m_Increment; }

        u64 m_State;
        u64 m_Increment;
    };
}
