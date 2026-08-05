#pragma once

#include <Veng/Veng.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace Veng::Audio::Dsp
{
    /// @brief A low-frequency modulator sharing the oscillator's morphable shape axis.
    ///
    /// The same continuous shape as @ref Oscillator (sine → triangle → saw → square) at sub-audio
    /// rate, for modulating a cutoff, an amplitude, or a delay time. It does @e not band-limit — its
    /// harmonics are inaudible, so it is a straight shape generator — but it shares the one waveform
    /// axis so a consumer reasons about a single shape vocabulary. The output is bipolar [-1, 1] or
    /// unipolar [0, 1] by a selector, and the starting phase is settable. Nothing allocates.
    class Lfo
    {
    public:
        /// @brief Sets the modulation frequency in Hz against a sample rate.
        /// @param hz          The frequency in Hz.
        /// @param sampleRate  The sample rate in Hz.
        void SetFrequency(f32 hz, u32 sampleRate)
        {
            m_Increment = sampleRate > 0 ? hz / static_cast<f32>(sampleRate) : 0.0f;
        }

        /// @brief Sets the waveform shape in [0, 1]: 0 sine, ~1/3 triangle, ~2/3 saw, 1 square.
        /// @param shape  The morph position in [0, 1].
        void SetShape(f32 shape) { m_Shape = std::clamp(shape, 0.0f, 1.0f); }

        /// @brief Sets the phase in [0, 1), wrapping any excess.
        /// @param phase  The normalised phase to jump to.
        void SetPhase(f32 phase)
        {
            f32 wrapped = phase - std::floor(phase);
            if (wrapped >= 1.0f)
            {
                wrapped -= 1.0f;
            }
            m_Phase = wrapped;
        }

        /// @brief Selects a unipolar [0, 1] output instead of the default bipolar [-1, 1].
        /// @param unipolar  True for a [0, 1] output, false for [-1, 1].
        void SetUnipolar(bool unipolar) { m_Unipolar = unipolar; }

        /// @brief Advances one sample and returns the next modulation value.
        ///
        /// The value is [-1, 1] bipolar, or [0, 1] when unipolar is selected.
        /// @return The next modulation value.
        [[nodiscard]] f32 Tick()
        {
            const f32 bipolar = Sample(m_Phase, m_Shape);
            m_Phase += m_Increment;
            while (m_Phase >= 1.0f)
            {
                m_Phase -= 1.0f;
            }
            return m_Unipolar ? bipolar * 0.5f + 0.5f : bipolar;
        }

    private:
        /// @brief Evaluates the naive (non-band-limited) morphed shape at a phase.
        /// @param phase  The normalised phase in [0, 1).
        /// @param shape  The morph position in [0, 1].
        /// @return The bipolar shape value in [-1, 1].
        [[nodiscard]] static f32 Sample(f32 phase, f32 shape)
        {
            const f32 sine = std::sin(2.0f * std::numbers::pi_v<f32> * phase);
            const f32 saw = (2.0f * phase) - 1.0f;
            const f32 square = phase < 0.5f ? 1.0f : -1.0f;
            const f32 triangle = phase < 0.5f ? (4.0f * phase) - 1.0f : 3.0f - (4.0f * phase);

            if (shape <= 1.0f / 3.0f)
            {
                const f32 t = shape * 3.0f;
                return sine + (triangle - sine) * t;
            }
            if (shape <= 2.0f / 3.0f)
            {
                const f32 t = (shape - (1.0f / 3.0f)) * 3.0f;
                return triangle + (saw - triangle) * t;
            }
            const f32 t = (shape - (2.0f / 3.0f)) * 3.0f;
            return saw + (square - saw) * t;
        }

        f32 m_Phase = 0.0f;      ///< @brief Normalised phase in [0, 1).
        f32 m_Increment = 0.0f;  ///< @brief Phase advance per sample.
        f32 m_Shape = 0.0f;      ///< @brief The morph position in [0, 1].
        bool m_Unipolar = false; ///< @brief Whether the output is remapped to [0, 1].
    };
}
