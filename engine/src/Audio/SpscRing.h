#pragma once

#include <Veng/Veng.h>

#include <array>
#include <atomic>

namespace Veng::Audio
{
    /// @brief A bounded lock-free single-producer / single-consumer ring queue.
    ///
    /// The real-time callback is the sole producer and the main thread the sole consumer (the
    /// reverse of the snapshot bridge): the callback posts finished voices and the main thread
    /// drains them, with no lock and no callback into engine state. A push onto a full ring is
    /// dropped rather than blocking — the main thread reconciles the truth from the snapshot, so a
    /// lost retirement is at worst a one-frame-late reap.
    ///
    /// @tparam T        A trivially-copyable payload.
    /// @tparam Capacity Ring capacity; must be a power of two.
    template <typename T, usize Capacity>
    class SpscRing
    {
    public:
        static_assert((Capacity & (Capacity - 1)) == 0, "Capacity must be a power of two");

        /// @brief Pushes an item (producer side); drops it if the ring is full.
        /// @param item The payload to enqueue.
        /// @return true if enqueued, false if the ring was full.
        bool Push(const T& item)
        {
            const usize head = m_Head.load(std::memory_order_relaxed);
            const usize next = (head + 1) & kMask;
            if (next == (m_Tail.load(std::memory_order_acquire) & kMask))
            {
                return false;
            }
            m_Slots[head] = item;
            m_Head.store(next, std::memory_order_release);
            return true;
        }

        /// @brief Free capacity as the producer sees it: items pushable before the ring is full.
        ///
        /// A producer-side query (loads its own head relaxed, the consumer's tail acquire), so it
        /// never underestimates space the consumer has already made — a subsequent Push of at most
        /// this many items is guaranteed to succeed. The usable capacity is Capacity - 1 (one slot
        /// is reserved to distinguish full from empty).
        /// @return The number of items that can be pushed before Push starts returning false.
        [[nodiscard]] usize Free() const
        {
            const usize head = m_Head.load(std::memory_order_relaxed);
            const usize tail = m_Tail.load(std::memory_order_acquire);
            return kMask - ((head - tail) & kMask);
        }

        /// @brief Items available to pop as the consumer sees it: poppable before the ring is empty.
        ///
        /// A consumer-side query (loads its own tail relaxed, the producer's head acquire), so it
        /// never overestimates what the producer has already published — a subsequent Pop of at most
        /// this many items is guaranteed to succeed. Used to drain a whole interleaved frame at once
        /// without half-popping a stereo pair on an underrun.
        /// @return The number of items that can be popped before Pop starts returning false.
        [[nodiscard]] usize Available() const
        {
            const usize head = m_Head.load(std::memory_order_acquire);
            const usize tail = m_Tail.load(std::memory_order_relaxed);
            return (head - tail) & kMask;
        }

        /// @brief Pops an item (consumer side).
        /// @param out Receives the dequeued payload when one was available.
        /// @return true if an item was dequeued, false if the ring was empty.
        bool Pop(T& out)
        {
            const usize tail = m_Tail.load(std::memory_order_relaxed);
            if (tail == m_Head.load(std::memory_order_acquire))
            {
                return false;
            }
            out = m_Slots[tail];
            m_Tail.store((tail + 1) & kMask, std::memory_order_release);
            return true;
        }

    private:
        /// @brief The index-wrap mask (Capacity - 1).
        static constexpr usize kMask = Capacity - 1;

        /// @brief The ring storage.
        std::array<T, Capacity> m_Slots{};
        /// @brief Producer write cursor.
        std::atomic<usize> m_Head{0};
        /// @brief Consumer read cursor.
        std::atomic<usize> m_Tail{0};
    };
}
