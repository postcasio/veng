#pragma once

#include <Veng/Veng.h>

#include <algorithm>
#include <cmath>
#include <numbers>

namespace Veng::Audio::Dsp
{
    /// @brief A band-limited oscillator whose waveform is a continuous shape parameter.
    ///
    /// The waveform is one continuous axis — sine at 0, triangle at ~1/3, saw at ~2/3, square at 1 —
    /// so a consumer interpolates @e between timbres rather than switching a discrete enum. The sine
    /// archetype is a plain phase accumulator; the non-sine archetypes are band-limited with PolyBLEP
    /// (and PolyBLAMP for the triangle's slope corners), so a swept oscillator does not alias into
    /// digital buzz. Each intermediate shape is a crossfade of two already band-limited archetypes,
    /// so every point on the axis is itself anti-aliased.
    ///
    /// All state is a phase accumulator and two scalars; nothing allocates, and @ref Tick is the
    /// per-sample advance a block loop calls @c frames times, so the oscillator makes no block-size
    /// assumption and is safe to drive on the real-time mixing thread.
    class Oscillator
    {
    public:
        /// @brief Sets the oscillation frequency in Hz against a sample rate.
        ///
        /// The per-sample phase increment is @p hz / @p sampleRate; a frequency of 0 holds a
        /// constant phase (a DC sample of the current shape). Retuning takes effect on the next
        /// @ref Tick, so a per-sample frequency sweep is simply a per-sample call.
        /// @param hz          The frequency in Hz.
        /// @param sampleRate  The output sample rate in Hz.
        void SetFrequency(f32 hz, u32 sampleRate)
        {
            m_Increment = sampleRate > 0 ? hz / static_cast<f32>(sampleRate) : 0.0f;
        }

        /// @brief Sets the waveform shape in [0, 1]: 0 sine, ~1/3 triangle, ~2/3 saw, 1 square.
        ///
        /// Values between the archetype anchors crossfade the two bracketing archetypes, so the axis
        /// is continuous. The value is clamped to [0, 1].
        /// @param shape  The morph position in [0, 1].
        void SetShape(f32 shape) { m_Shape = std::clamp(shape, 0.0f, 1.0f); }

        /// @brief Resets the phase accumulator to zero.
        void Reset() { m_Phase = 0.0f; }

        /// @brief Advances one sample and returns the next value in [-1, 1].
        ///
        /// The value is sampled at the current phase before the phase advances by the increment, so a
        /// block of @c N calls produces @c N contiguous samples with no seam. Band-limited archetypes
        /// may overshoot the nominal [-1, 1] range by a small amount at a discontinuity (the expected
        /// PolyBLEP behaviour).
        /// @return The next oscillator sample.
        [[nodiscard]] f32 Tick()
        {
            const f32 value = Sample(m_Phase, m_Increment, m_Shape);
            m_Phase += m_Increment;
            while (m_Phase >= 1.0f)
            {
                m_Phase -= 1.0f;
            }
            return value;
        }

    private:
        /// @brief The two-sample PolyBLEP residual correcting a unit step discontinuity.
        ///
        /// Returns a smooth correction over the two samples straddling a phase-wrap step, zero
        /// elsewhere, band-limiting the naive step of a saw or square.
        /// @param t   The normalised phase in [0, 1).
        /// @param dt  The per-sample phase increment.
        /// @return The step-correction residual.
        [[nodiscard]] static f32 PolyBlep(f32 t, f32 dt)
        {
            if (dt <= 0.0f)
            {
                return 0.0f;
            }
            if (t < dt)
            {
                const f32 x = t / dt;
                return (x + x) - (x * x) - 1.0f;
            }
            if (t > 1.0f - dt)
            {
                const f32 x = (t - 1.0f) / dt;
                return (x * x) + (x + x) + 1.0f;
            }
            return 0.0f;
        }

        /// @brief The two-sample PolyBLAMP residual correcting a unit slope discontinuity.
        ///
        /// The integral of @ref PolyBlep: it band-limits a corner (a change of slope) rather than a
        /// step, which is what the triangle's two corners need.
        /// @param t   The normalised phase in [0, 1).
        /// @param dt  The per-sample phase increment.
        /// @return The slope-correction residual.
        [[nodiscard]] static f32 PolyBlamp(f32 t, f32 dt)
        {
            if (dt <= 0.0f)
            {
                return 0.0f;
            }
            if (t < dt)
            {
                const f32 x = (t / dt) - 1.0f;
                return -(1.0f / 3.0f) * x * x * x;
            }
            if (t > 1.0f - dt)
            {
                const f32 x = ((t - 1.0f) / dt) + 1.0f;
                return (1.0f / 3.0f) * x * x * x;
            }
            return 0.0f;
        }

        /// @brief Evaluates a band-limited archetype value for a phase, increment, and shape.
        ///
        /// Computes the four archetype samples at @p phase and crossfades the two the shape brackets.
        /// @param phase  The normalised phase in [0, 1).
        /// @param dt     The per-sample phase increment (the PolyBLEP width).
        /// @param shape  The morph position in [0, 1].
        /// @return The morphed, band-limited sample.
        [[nodiscard]] static f32 Sample(f32 phase, f32 dt, f32 shape)
        {
            const f32 sine = std::sin(2.0f * std::numbers::pi_v<f32> * phase);

            f32 half = phase + 0.5f;
            if (half >= 1.0f)
            {
                half -= 1.0f;
            }

            f32 saw = (2.0f * phase) - 1.0f;
            saw -= PolyBlep(phase, dt);

            f32 square = phase < 0.5f ? 1.0f : -1.0f;
            square += PolyBlep(phase, dt);
            square -= PolyBlep(half, dt);

            f32 triangle = phase < 0.5f ? (4.0f * phase) - 1.0f : 3.0f - (4.0f * phase);
            triangle += 8.0f * dt * PolyBlamp(phase, dt);
            triangle -= 8.0f * dt * PolyBlamp(half, dt);

            // The shape axis is three equal segments between four archetype anchors.
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

        f32 m_Phase = 0.0f;     ///< @brief Normalised phase in [0, 1).
        f32 m_Increment = 0.0f; ///< @brief Phase advance per sample.
        f32 m_Shape = 0.0f;     ///< @brief The morph position in [0, 1].
    };
}
