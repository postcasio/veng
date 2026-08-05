#pragma once

#include <Veng/Assert.h>
#include <Veng/Veng.h>

namespace Veng::Audio::Dsp
{
    /// @brief A fractional delay line over a ring buffer sized once in @ref Prepare.
    ///
    /// The storage primitive an echo, a chorus, or a reverb's comb / all-pass bank is built on. The
    /// maximum length is allocated a single time in @ref Prepare — never on the real-time thread —
    /// after which @ref Write, @ref Read, and the fractional @ref ReadInterpolated are all
    /// allocation-free. Reads are measured backward from the most recent write: a delay of 1 is the
    /// just-written sample.
    class DelayLine
    {
    public:
        /// @brief Allocates the ring to hold @p maxSamples and clears it.
        ///
        /// The one allocating call; run it off the real-time thread before any @ref Write. A capacity
        /// of 0 is rejected — a delay line needs at least one slot.
        /// @param maxSamples  The maximum delay length in samples (the ring capacity).
        void Prepare(u32 maxSamples)
        {
            VE_ASSERT(maxSamples > 0, "DelayLine::Prepare needs a non-zero capacity");
            m_Buffer.assign(maxSamples, 0.0f);
            m_Write = 0;
        }

        /// @brief Zeroes the ring without reallocating.
        void Clear()
        {
            for (f32& sample : m_Buffer)
            {
                sample = 0.0f;
            }
            m_Write = 0;
        }

        /// @brief The ring capacity in samples (the maximum delay).
        /// @return The number of slots allocated by @ref Prepare.
        [[nodiscard]] u32 Capacity() const { return static_cast<u32>(m_Buffer.size()); }

        /// @brief Writes one sample at the head and advances the write cursor.
        /// @param sample  The sample to store.
        void Write(f32 sample)
        {
            VE_ASSERT(!m_Buffer.empty(), "DelayLine::Write before Prepare");
            m_Buffer[m_Write] = sample;
            m_Write = (m_Write + 1) % m_Buffer.size();
        }

        /// @brief Reads the sample written @p delaySamples writes ago.
        ///
        /// A delay of 1 returns the most recently written sample, 2 the one before it, and so on up to
        /// the capacity.
        /// @param delaySamples  The integer delay in samples, in [1, Capacity()].
        /// @return The delayed sample.
        [[nodiscard]] f32 Read(u32 delaySamples) const
        {
            VE_ASSERT(delaySamples >= 1 && delaySamples <= m_Buffer.size(),
                      "DelayLine::Read delay {} out of range [1, {}]", delaySamples,
                      m_Buffer.size());
            const usize size = m_Buffer.size();
            const usize index = (m_Write + size - delaySamples) % size;
            return m_Buffer[index];
        }

        /// @brief Reads with a fractional delay, linearly interpolating the two straddling samples.
        ///
        /// The same two-sample window the rest of the subsystem resamples through. The delay is split
        /// into its integer floor and fraction; the floor must be at least 1 and the floor plus one
        /// must not exceed the capacity.
        /// @param delaySamples  The fractional delay in samples, in [1, Capacity() - 1].
        /// @return The interpolated delayed sample.
        [[nodiscard]] f32 ReadInterpolated(f32 delaySamples) const
        {
            const u32 whole = static_cast<u32>(delaySamples);
            const f32 frac = delaySamples - static_cast<f32>(whole);
            const f32 s0 = Read(whole);
            const f32 s1 = Read(whole + 1);
            return s0 + (s1 - s0) * frac;
        }

    private:
        vector<f32> m_Buffer; ///< @brief The ring storage, sized by Prepare.
        usize m_Write = 0;    ///< @brief The next slot a Write fills.
    };
}
