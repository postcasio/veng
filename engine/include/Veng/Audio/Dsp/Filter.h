#pragma once

#include <Veng/Veng.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace Veng::Audio::Dsp
{
    /// @brief A resonant TPT state-variable filter yielding LP/HP/BP/notch from one core.
    ///
    /// Zavalishin's topology-preserving-transform state-variable filter: one core with two integrator
    /// states produces low-pass, high-pass, band-pass, and notch from the same processed sample, with
    /// a resonance (Q) control. Being TPT, it is unconditionally stable and stays bounded under
    /// per-sample cutoff modulation — the case a filter sweep @e is — which the subsystem's one-pole
    /// low-pass (no resonance, one response) cannot give. Nothing allocates; it runs on the real-time
    /// thread.
    class Filter
    {
    public:
        /// @brief The four simultaneous responses of one processed sample.
        struct Outputs
        {
            f32 LowPass;  ///< @brief The low-pass response.
            f32 HighPass; ///< @brief The high-pass response.
            f32 BandPass; ///< @brief The band-pass response.
            f32 Notch;    ///< @brief The notch (band-reject) response.
        };

        /// @brief Sets the cutoff frequency in Hz against a sample rate.
        ///
        /// Stores the prewarped coefficient @c tan(pi * fc / fs); the cutoff is clamped just inside
        /// (0, Nyquist) so the prewarp stays finite. Retuning takes effect on the next @ref Tick, so
        /// a per-sample sweep is a per-sample call and stays stable.
        /// @param hz          The cutoff frequency in Hz.
        /// @param sampleRate  The sample rate in Hz.
        void SetCutoff(f32 hz, u32 sampleRate)
        {
            const f32 nyquist = static_cast<f32>(sampleRate) * 0.5f;
            const f32 clamped = std::clamp(hz, 1.0f, nyquist - 1.0f);
            m_G = std::tan(std::numbers::pi_v<f32> * clamped / static_cast<f32>(sampleRate));
        }

        /// @brief Sets the resonance (Q); higher is a sharper peak at cutoff.
        ///
        /// The damping is 1/Q, so a larger @p q lowers the damping and sharpens the resonant peak. Q
        /// is clamped to a sane band so the filter cannot self-oscillate into a blow-up.
        /// @param q  The quality factor; clamped to [0.5, 40].
        void SetResonance(f32 q) { m_K = 1.0f / std::clamp(q, 0.5f, 40.0f); }

        /// @brief Resets both integrator states to zero.
        void Reset()
        {
            m_S1 = 0.0f;
            m_S2 = 0.0f;
        }

        /// @brief Advances one sample and returns all four responses.
        /// @param input  The input sample.
        /// @return The four simultaneous responses.
        [[nodiscard]] Outputs Tick(f32 input)
        {
            const f32 a1 = 1.0f / (1.0f + m_G * (m_G + m_K));
            const f32 a2 = m_G * a1;
            const f32 a3 = m_G * a2;

            const f32 v3 = input - m_S2;
            const f32 v1 = a1 * m_S1 + a2 * v3;
            const f32 v2 = m_S2 + a2 * m_S1 + a3 * v3;
            m_S1 = 2.0f * v1 - m_S1;
            m_S2 = 2.0f * v2 - m_S2;

            const f32 low = v2;
            const f32 band = v1;
            const f32 high = input - m_K * v1 - v2;
            return Outputs{.LowPass = low, .HighPass = high, .BandPass = band, .Notch = low + high};
        }

    private:
        f32 m_G = 0.0f;  ///< @brief Prewarped cutoff coefficient tan(pi * fc / fs).
        f32 m_K = 1.0f;  ///< @brief Damping, 1/Q.
        f32 m_S1 = 0.0f; ///< @brief First integrator state.
        f32 m_S2 = 0.0f; ///< @brief Second integrator state.
    };
}
