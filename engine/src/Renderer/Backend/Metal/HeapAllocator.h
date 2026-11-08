#pragma once

// The Metal backend's placement-heap sub-allocator, device-free. The backend creates placement heaps
// and places each buffer and texture at an offset this chooses; the free space of one heap is a
// sorted run of ranges, allocated first fit and coalesced on free.

#include <Veng/Veng.h>

#include <Veng/Assert.h>

namespace Veng::Renderer::Metal
{
    /// @brief The size of a shared placement heap; a request larger than a quarter of it gets a heap
    ///        of its own.
    inline constexpr u64 HeapBlockBytes = 64ull * 1024 * 1024;

    /// @brief The granularity a dedicated heap is rounded up to.
    inline constexpr u64 HeapPageBytes = 64ull * 1024;

    /// @brief Whether a placement of @p size bytes takes a heap of its own rather than sharing one.
    /// @param size The placement's size, as the device reports it.
    /// @return True when the placement is dedicated.
    [[nodiscard]] constexpr bool IsDedicatedPlacement(const u64 size)
    {
        return size > HeapBlockBytes / 4;
    }

    /// @brief The size of the heap a placement of @p size bytes is made in.
    /// @param size The placement's size, as the device reports it.
    /// @return HeapBlockBytes for a shared heap, or @p size rounded up to HeapPageBytes for a
    ///         dedicated one.
    [[nodiscard]] constexpr u64 HeapBytesFor(const u64 size)
    {
        if (!IsDedicatedPlacement(size))
        {
            return HeapBlockBytes;
        }
        return (size + HeapPageBytes - 1) / HeapPageBytes * HeapPageBytes;
    }

    /// @brief The free space of one heap: first-fit placement at an alignment, coalescing frees.
    class HeapBlockAllocator
    {
    public:
        /// @brief Starts with the whole heap free.
        /// @param size The heap's size in bytes.
        explicit HeapBlockAllocator(const u64 size) : m_Size(size) { m_Free.emplace(0, size); }

        /// @brief Places @p size bytes at the lowest offset aligned to @p alignment that fits.
        /// @param size      The placement's size in bytes; nonzero.
        /// @param alignment The placement's alignment; a power of two.
        /// @return The offset, or nullopt when no free range fits.
        [[nodiscard]] optional<u64> Allocate(const u64 size, const u64 alignment)
        {
            VE_ASSERT(size > 0, "HeapBlockAllocator: a placement of zero bytes");
            VE_ASSERT(alignment > 0 && (alignment & (alignment - 1)) == 0,
                      "HeapBlockAllocator: alignment {} is not a power of two", alignment);
            for (auto it = m_Free.begin(); it != m_Free.end(); ++it)
            {
                const u64 rangeStart = it->first;
                const u64 rangeEnd = it->first + it->second;
                const u64 placed = (rangeStart + alignment - 1) & ~(alignment - 1);
                if (placed + size > rangeEnd)
                {
                    continue;
                }
                m_Free.erase(it);
                if (placed > rangeStart)
                {
                    m_Free.emplace(rangeStart, placed - rangeStart);
                }
                if (placed + size < rangeEnd)
                {
                    m_Free.emplace(placed + size, rangeEnd - (placed + size));
                }
                m_Used += size;
                return placed;
            }
            return std::nullopt;
        }

        /// @brief Returns a placement's range to the free space, merging it with its neighbours.
        /// @param offset The offset Allocate returned.
        /// @param size   The size it was allocated with.
        void Free(const u64 offset, const u64 size)
        {
            VE_ASSERT(offset + size <= m_Size && size <= m_Used,
                      "HeapBlockAllocator: freeing [{}, {}) that was never placed", offset,
                      offset + size);
            u64 start = offset;
            u64 end = offset + size;
            const auto next = m_Free.lower_bound(start);
            if (next != m_Free.end())
            {
                VE_ASSERT(next->first >= end, "HeapBlockAllocator: [{}, {}) is already free", start,
                          end);
                if (next->first == end)
                {
                    end += next->second;
                    m_Free.erase(next);
                }
            }
            const auto after = m_Free.lower_bound(start);
            if (after != m_Free.begin())
            {
                const auto previous = std::prev(after);
                VE_ASSERT(previous->first + previous->second <= start,
                          "HeapBlockAllocator: [{}, {}) is already free", start, end);
                if (previous->first + previous->second == start)
                {
                    start = previous->first;
                    m_Free.erase(previous);
                }
            }
            m_Free.emplace(start, end - start);
            m_Used -= size;
        }

        /// @brief The heap's size in bytes.
        [[nodiscard]] u64 GetSize() const { return m_Size; }

        /// @brief The bytes currently placed.
        [[nodiscard]] u64 GetUsed() const { return m_Used; }

        /// @brief Whether nothing is placed.
        [[nodiscard]] bool IsEmpty() const { return m_Used == 0; }

        /// @brief The number of disjoint free ranges.
        [[nodiscard]] usize GetFreeRangeCount() const { return m_Free.size(); }

    private:
        /// @brief The heap's size in bytes.
        u64 m_Size = 0;
        /// @brief The bytes currently placed.
        u64 m_Used = 0;
        /// @brief Free ranges, keyed by offset, valued by size; never adjacent.
        map<u64, u64> m_Free;
    };
}
