#pragma once

#include <Veng/Veng.h>

#include <atomic>

namespace Veng::Audio
{
    /// @brief A lock-free single-producer / single-consumer triple buffer.
    ///
    /// The producer (main thread) writes a back buffer and publishes it; the consumer (the
    /// real-time callback) latches the newest published buffer and holds it for a whole block.
    /// Three slots guarantee the producer never has to reclaim the slot the consumer holds, so a
    /// hard-real-time reader never spins, never blocks, and never sees a torn frame — the property
    /// double buffering cannot give without a seqlock. Neither side allocates.
    ///
    /// @tparam T A POD frame type; three are held resident.
    template <typename T>
    class TripleBuffer
    {
    public:
        TripleBuffer() = default;

        /// @brief Returns the back buffer the producer may write.
        [[nodiscard]] T& BackBuffer() { return m_Buffers[m_WriteIndex]; }

        /// @brief Publishes the back buffer, making it the newest, and takes a fresh back buffer.
        ///
        /// The exchange stores the just-written slot (flagged fresh) into the shared middle and
        /// takes the old middle as the next back buffer, so the producer's back slot, the shared
        /// middle, and the consumer's front slot stay three distinct indices.
        void Publish()
        {
            const u32 old = m_Middle.exchange(m_WriteIndex | kDirtyBit, std::memory_order_acq_rel);
            m_WriteIndex = old & kIndexMask;
        }

        /// @brief Latches the newest published buffer if one arrived since the last latch.
        /// @return true if a newer buffer was latched, false if none was published since.
        bool FetchNewest()
        {
            if ((m_Middle.load(std::memory_order_acquire) & kDirtyBit) == 0)
            {
                return false;
            }
            const u32 old = m_Middle.exchange(m_ReadIndex, std::memory_order_acq_rel);
            m_ReadIndex = old & kIndexMask;
            return true;
        }

        /// @brief Returns the front buffer the consumer currently holds.
        [[nodiscard]] const T& FrontBuffer() const { return m_Buffers[m_ReadIndex]; }

    private:
        /// @brief The fresh-data flag packed above the two index bits.
        static constexpr u32 kDirtyBit = 0x4U;
        /// @brief The mask selecting the buffer index (three slots need two bits).
        static constexpr u32 kIndexMask = 0x3U;

        /// @brief The three resident frames.
        T m_Buffers[3] = {};
        /// @brief Producer-owned index of the writable back buffer.
        u32 m_WriteIndex = 0;
        /// @brief Consumer-owned index of the readable front buffer.
        u32 m_ReadIndex = 1;
        /// @brief The shared middle slot plus the fresh-data flag; starts on the third index.
        std::atomic<u32> m_Middle{2};
    };
}
