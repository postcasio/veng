#pragma once

#include <Veng/Veng.h>

#include <algorithm>

namespace Veng::Audio::Dsp
{
    /// @brief A four-segment ADSR envelope that advances purely by sample count.
    ///
    /// Attack rises 0 → 1, decay falls to the sustain level, sustain holds until release, and release
    /// falls to 0. It is driven by @ref NoteOn / @ref NoteOff and advanced one sample per @ref Tick
    /// (or a whole run by @ref Advance), returning the current gain in [0, 1]. Segment lengths are
    /// held in @e samples — a seconds setter converts against the sample rate — so it assumes no
    /// fixed block size and never allocates. @ref IsActive lets a consumer retire a voice once the
    /// release completes.
    class Envelope
    {
    public:
        /// @brief Sets the attack length in samples (0 snaps instantly to full gain).
        /// @param samples  The attack length in samples.
        void SetAttack(f32 samples) { m_Attack = samples > 0.0f ? samples : 0.0f; }

        /// @brief Sets the decay length in samples (0 snaps instantly to the sustain level).
        /// @param samples  The decay length in samples.
        void SetDecay(f32 samples) { m_Decay = samples > 0.0f ? samples : 0.0f; }

        /// @brief Sets the sustain gain in [0, 1] held between decay and release.
        /// @param level  The sustain level, clamped to [0, 1].
        void SetSustain(f32 level) { m_Sustain = std::clamp(level, 0.0f, 1.0f); }

        /// @brief Sets the release length in samples (0 snaps instantly to silence).
        /// @param samples  The release length in samples.
        void SetRelease(f32 samples) { m_Release = samples > 0.0f ? samples : 0.0f; }

        /// @brief Sets all four segments at once, the times in seconds against a sample rate.
        ///
        /// A convenience over the per-segment setters; the times convert to sample counts against
        /// @p sampleRate.
        /// @param attackSeconds   Attack length in seconds.
        /// @param decaySeconds    Decay length in seconds.
        /// @param sustainLevel    Sustain gain in [0, 1].
        /// @param releaseSeconds  Release length in seconds.
        /// @param sampleRate      The sample rate the times convert against, in Hz.
        void SetSeconds(f32 attackSeconds, f32 decaySeconds, f32 sustainLevel, f32 releaseSeconds,
                        u32 sampleRate)
        {
            const f32 rate = static_cast<f32>(sampleRate);
            SetAttack(attackSeconds * rate);
            SetDecay(decaySeconds * rate);
            SetSustain(sustainLevel);
            SetRelease(releaseSeconds * rate);
        }

        /// @brief Begins the attack segment from the current gain.
        ///
        /// Starting from the current value (rather than a hard 0) means a re-triggered envelope does
        /// not click: the attack resumes at whatever gain is held, covering only the remaining rise.
        /// The envelope becomes @ref IsActive.
        void NoteOn()
        {
            m_Stage = Stage::Attack;
            m_Position = m_Value * m_Attack; // resume the ramp at the current gain
        }

        /// @brief Begins the release segment from the current gain.
        ///
        /// A note off during any stage releases from wherever the gain currently is, reaching silence
        /// over the full release length.
        void NoteOff()
        {
            if (m_Stage != Stage::Idle)
            {
                m_Stage = Stage::Release;
                m_ReleaseStart = m_Value;
                m_Position = 0.0f;
            }
        }

        /// @brief Advances one sample and returns the current gain in [0, 1].
        ///
        /// Each timed segment advances by an exact per-sample count, so timing is exact and the gain
        /// is monotone within a segment — no float-accumulation drift across a boundary.
        /// @return The gain after this sample.
        [[nodiscard]] f32 Tick()
        {
            switch (m_Stage)
            {
            case Stage::Idle:
            {
                break;
            }
            case Stage::Attack:
            {
                if (m_Attack <= 0.0f)
                {
                    m_Value = 1.0f;
                    EnterDecay();
                    break;
                }
                m_Position += 1.0f;
                m_Value = m_Position / m_Attack;
                if (m_Position >= m_Attack)
                {
                    m_Value = 1.0f;
                    EnterDecay();
                }
                break;
            }
            case Stage::Decay:
            {
                if (m_Decay <= 0.0f)
                {
                    m_Value = m_Sustain;
                    m_Stage = Stage::Sustain;
                    break;
                }
                m_Position += 1.0f;
                m_Value = 1.0f - (1.0f - m_Sustain) * (m_Position / m_Decay);
                if (m_Position >= m_Decay)
                {
                    m_Value = m_Sustain;
                    m_Stage = Stage::Sustain;
                }
                break;
            }
            case Stage::Sustain:
            {
                m_Value = m_Sustain;
                break;
            }
            case Stage::Release:
            {
                if (m_Release <= 0.0f)
                {
                    m_Value = 0.0f;
                    m_Stage = Stage::Idle;
                    break;
                }
                m_Position += 1.0f;
                m_Value = m_ReleaseStart * (1.0f - (m_Position / m_Release));
                if (m_Position >= m_Release)
                {
                    m_Value = 0.0f;
                    m_Stage = Stage::Idle;
                }
                break;
            }
            }
            return m_Value;
        }

        /// @brief Advances @p frames samples and returns the gain after the last one.
        ///
        /// Equivalent to calling @ref Tick @p frames times; a consumer that only needs the endpoint
        /// gain (a per-block amplitude) advances the whole block at once.
        /// @param frames  The number of samples to advance.
        /// @return The gain after the final advanced sample (the current gain if @p frames is 0).
        f32 Advance(u32 frames)
        {
            for (u32 i = 0; i < frames; ++i)
            {
                static_cast<void>(Tick());
            }
            return m_Value;
        }

        /// @brief Returns the current gain without advancing.
        /// @return The current gain in [0, 1].
        [[nodiscard]] f32 Value() const { return m_Value; }

        /// @brief Returns whether the envelope is producing gain (not idle).
        ///
        /// False before the first @ref NoteOn and once a release has fallen to silence — the signal a
        /// consumer retires a voice on.
        /// @return True while any segment is running.
        [[nodiscard]] bool IsActive() const { return m_Stage != Stage::Idle; }

    private:
        /// @brief Enters the decay segment from the top of attack, resetting the segment position.
        void EnterDecay()
        {
            m_Stage = Stage::Decay;
            m_Position = 0.0f;
        }

        /// @brief The segment the envelope is currently running.
        enum class Stage : u8
        {
            Idle,    ///< @brief Silent and inactive; the pre-NoteOn / post-release state.
            Attack,  ///< @brief Rising toward full gain.
            Decay,   ///< @brief Falling toward the sustain level.
            Sustain, ///< @brief Holding the sustain level until NoteOff.
            Release, ///< @brief Falling toward silence.
        };

        f32 m_Attack = 0.0f;       ///< @brief Attack length in samples.
        f32 m_Decay = 0.0f;        ///< @brief Decay length in samples.
        f32 m_Sustain = 1.0f;      ///< @brief Sustain gain in [0, 1].
        f32 m_Release = 0.0f;      ///< @brief Release length in samples.
        f32 m_Value = 0.0f;        ///< @brief The current gain in [0, 1].
        f32 m_Position = 0.0f;     ///< @brief Samples elapsed in the current timed segment.
        f32 m_ReleaseStart = 0.0f; ///< @brief The gain captured at NoteOff, the release ramps from.
        Stage m_Stage = Stage::Idle; ///< @brief The active segment.
    };
}
