#pragma once

#include <Veng/Veng.h>

namespace Veng::Audio::Dsp
{
    /// @brief A deterministic white/pink noise source over a seeded xorshift PRNG.
    ///
    /// Carries a small integer PRNG state so a fixed seed reproduces a sample sequence exactly (what
    /// a test pins), and a short filter bank turning the same white draws into pink (≈ -3 dB/octave)
    /// noise. @ref White and @ref Pink each draw exactly one white sample per call, so interleaving
    /// them stays deterministic. Nothing allocates; it is safe to draw on the real-time thread.
    class Noise
    {
    public:
        /// @brief Constructs a noise source with the default seed.
        Noise() = default;

        /// @brief Constructs a noise source seeded to @p seed.
        /// @param seed  The initial PRNG state (0 is remapped to a non-zero constant).
        explicit Noise(u32 seed) { SetSeed(seed); }

        /// @brief Reseeds the PRNG, resetting the reproducible sequence.
        ///
        /// The xorshift generator has no valid all-zero state, so a seed of 0 is remapped to a fixed
        /// non-zero constant. The pink filter state is left untouched (it re-converges within a few
        /// samples); reseed and draw from @ref White for a bit-reproducible run.
        /// @param seed  The new PRNG state.
        void SetSeed(u32 seed) { m_State = seed != 0 ? seed : 0x1u; }

        /// @brief Draws the next white sample in [-1, 1].
        ///
        /// White noise: a flat expected spectrum and a near-zero mean. Advances the PRNG once.
        /// @return The next white sample.
        [[nodiscard]] f32 White() { return NextWhite(); }

        /// @brief Draws the next pink sample in approximately [-1, 1].
        ///
        /// Pink noise: an expected spectrum falling ≈ -3 dB/octave, filtered from one fresh white
        /// draw through the Paul Kellet refined bank. Advances the PRNG once.
        /// @return The next pink sample.
        [[nodiscard]] f32 Pink()
        {
            const f32 white = NextWhite();
            m_B0 = 0.99886f * m_B0 + white * 0.0555179f;
            m_B1 = 0.99332f * m_B1 + white * 0.0750759f;
            m_B2 = 0.96900f * m_B2 + white * 0.1538520f;
            m_B3 = 0.86650f * m_B3 + white * 0.3104856f;
            m_B4 = 0.55000f * m_B4 + white * 0.5329522f;
            m_B5 = -0.7616f * m_B5 - white * 0.0168980f;
            const f32 pink = m_B0 + m_B1 + m_B2 + m_B3 + m_B4 + m_B5 + m_B6 + white * 0.5362f;
            m_B6 = white * 0.115926f;
            // The bank sums to roughly ±3.5; scale back toward the [-1, 1] convention.
            return pink * 0.2f;
        }

    private:
        /// @brief Advances the xorshift PRNG and maps it to a white sample in [-1, 1].
        /// @return The next white sample.
        [[nodiscard]] f32 NextWhite()
        {
            m_State ^= m_State << 13;
            m_State ^= m_State >> 17;
            m_State ^= m_State << 5;
            // Map the top 24 bits to [-1, 1) so the mantissa is filled exactly.
            const u32 bits = m_State >> 8;
            return (static_cast<f32>(bits) / 8388608.0f) - 1.0f;
        }

        u32 m_State = 0x1234567u; ///< @brief The xorshift PRNG state (never zero).
        f32 m_B0 = 0.0f;          ///< @brief Pink filter pole 0.
        f32 m_B1 = 0.0f;          ///< @brief Pink filter pole 1.
        f32 m_B2 = 0.0f;          ///< @brief Pink filter pole 2.
        f32 m_B3 = 0.0f;          ///< @brief Pink filter pole 3.
        f32 m_B4 = 0.0f;          ///< @brief Pink filter pole 4.
        f32 m_B5 = 0.0f;          ///< @brief Pink filter pole 5.
        f32 m_B6 = 0.0f;          ///< @brief Pink filter pole 6.
    };
}
