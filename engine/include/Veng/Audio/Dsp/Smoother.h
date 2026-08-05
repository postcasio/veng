#pragma once

#include <Veng/Veng.h>

#include <cmath>

namespace Veng::Audio::Dsp
{
    /// @brief A one-pole slew that eases a control value toward a target without zippering.
    ///
    /// The control-rate sibling of the audio-rate one-pole: a stepped parameter (a cutoff jump, a
    /// gain change latched from a @c GeneratorParams block) reaches the DSP smoothly instead of as an
    /// audible click. @ref SetTarget names where to go, @ref SetTime the exponential time constant,
    /// and @ref Tick advances one step toward the target — monotonically, never overshooting. Nothing
    /// allocates; it runs on the real-time thread.
    class Smoother
    {
    public:
        /// @brief Sets the value the slew eases toward.
        /// @param target  The new target value.
        void SetTarget(f32 target) { m_Target = target; }

        /// @brief Snaps the current value (and the target) to @p value with no slew.
        ///
        /// Seeds the slew so the first block does not ramp up from zero — the honest way to
        /// initialise a smoother to a known parameter value.
        /// @param value  The value to jump to.
        void SetValue(f32 value)
        {
            m_Value = value;
            m_Target = value;
        }

        /// @brief Sets the smoothing time constant in seconds against a sample rate.
        ///
        /// The per-sample pole is @c exp(-1 / (seconds * sampleRate)); after @p seconds the value has
        /// closed roughly 63% of the gap, and it settles within a small tolerance after a few time
        /// constants. A non-positive time makes the slew instantaneous (the value jumps to the target
        /// on the next @ref Tick).
        /// @param seconds     The time constant in seconds.
        /// @param sampleRate  The sample rate in Hz.
        void SetTime(f32 seconds, u32 sampleRate)
        {
            const f32 samples = seconds * static_cast<f32>(sampleRate);
            m_Coefficient = samples > 0.0f ? std::exp(-1.0f / samples) : 0.0f;
        }

        /// @brief Advances one sample toward the target and returns the new value.
        /// @return The eased value after this sample.
        [[nodiscard]] f32 Tick()
        {
            m_Value = m_Target + (m_Value - m_Target) * m_Coefficient;
            return m_Value;
        }

        /// @brief Returns the current value without advancing.
        /// @return The current eased value.
        [[nodiscard]] f32 Value() const { return m_Value; }

        /// @brief Returns the value the slew is easing toward.
        /// @return The current target value.
        [[nodiscard]] f32 Target() const { return m_Target; }

    private:
        f32 m_Value = 0.0f;       ///< @brief The current eased value.
        f32 m_Target = 0.0f;      ///< @brief The value being eased toward.
        f32 m_Coefficient = 0.0f; ///< @brief The per-sample one-pole retention coefficient.
    };
}
