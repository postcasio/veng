#pragma once

#include <Veng/Veng.h>

namespace Veng::Renderer
{
    /// @brief A first-fit byte suballocator over one region of the material parameter arena.
    ///
    /// Each material owns one contiguous byte range, allocated once and identical in every
    /// frame-in-flight region, so the allocator reasons about a single region's bytes and the
    /// registry adds the region base. Device-free and I/O-free: it holds the free runs, the
    /// per-frame pending buckets, and two counters, and touches no buffer.
    ///
    /// Every request is rounded up to a whole number of GranuleBytes granules, which is what makes
    /// first fit exact rather than hopeful here: no gap can be an unusable sliver, and a freed run
    /// of k granules is exactly the size of the next request for k granules, so a pool cycling one
    /// material class through one range reuses its own hole every time. Internal waste is at most
    /// one granule less a byte per material.
    ///
    /// A freed range carries the same deferred window the slot allocators apply: ReleaseDeferred
    /// parks it in the bucket of the frame-in-flight current at release, and OnFrameAcquired
    /// returns that bucket to the free list once the frame comes round again — by which point the
    /// fence has been waited and no recorded draw still reads the range. Neighbours coalesce at
    /// that reclaim and never at release, because a range merged into the free list at release is
    /// allocatable immediately, which is what the window exists to prevent.
    class MaterialArena
    {
    public:
        /// @brief One contiguous byte range of the arena.
        struct Range
        {
            /// @brief The range's byte offset from the region base.
            u32 Offset = 0;
            /// @brief The range's byte length, always a granule multiple.
            u32 Bytes = 0;
        };

        /// @brief The allocation quantum: every range's offset and length is a multiple of this.
        static constexpr u32 GranuleBytes = 256;

        /// @brief Sizes the arena and its per-frame pending buckets, and resets it to one free run.
        /// @param arenaBytes     The region's byte capacity; rounded down to a granule multiple.
        /// @param framesInFlight The number of deferred-release buckets to keep.
        void Init(u32 arenaBytes, u32 framesInFlight);

        /// @brief The granule-rounded length a request of @p bytes actually occupies.
        ///
        /// A zero-byte block still takes one granule: a fieldless material is a distinct
        /// allocation, and two of them sharing an offset would make their handles equal.
        /// @param bytes The requested length.
        /// @return The occupied length, a positive granule multiple.
        [[nodiscard]] static u32 OccupiedBytes(u32 bytes);

        /// @brief Allocates the first free run long enough for @p bytes, rounded to a granule.
        /// @param bytes The requested length.
        /// @return The range's byte offset, or nothing when no free run is long enough.
        [[nodiscard]] optional<u32> Allocate(u32 bytes);

        /// @brief Parks a range for reclaim once frame @p currentFrameInFlight comes round again.
        /// @param offset               The range's offset, as Allocate returned it.
        /// @param bytes                The range's occupied length.
        /// @param currentFrameInFlight The frame-in-flight current at the release.
        void ReleaseDeferred(u32 offset, u32 bytes, u32 currentFrameInFlight);

        /// @brief Returns @p frameInFlight's parked ranges to the free list, coalescing neighbours.
        void OnFrameAcquired(u32 frameInFlight);

        /// @brief The region's byte capacity.
        [[nodiscard]] u32 GetArenaBytes() const { return m_ArenaBytes; }

        /// @brief The bytes occupied by live allocations, granule rounding included.
        [[nodiscard]] u32 GetLiveBytes() const { return m_LiveBytes; }

        /// @brief The number of live allocations.
        [[nodiscard]] u32 GetLiveBlocks() const { return m_LiveBlocks; }

        /// @brief The bytes on the free list — neither live nor waiting out a release window.
        [[nodiscard]] u32 GetFreeBytes() const;

        /// @brief The longest single free run, which is the largest allocation that can succeed.
        [[nodiscard]] u32 GetLargestFreeRun() const;

        /// @brief The free runs, ascending by offset and never adjacent to one another.
        [[nodiscard]] const vector<Range>& GetFreeRuns() const { return m_Free; }

        /// @brief The parked ranges, one bucket per frame-in-flight.
        [[nodiscard]] const vector<vector<Range>>& GetPendingRuns() const
        {
            return m_PendingRelease;
        }

    private:
        /// @brief Inserts a reclaimed range in offset order, merging it with either neighbour.
        void Insert(Range range);

        /// @brief The region's byte capacity.
        u32 m_ArenaBytes = 0;
        /// @brief The bytes occupied by live allocations.
        u32 m_LiveBytes = 0;
        /// @brief The number of live allocations.
        u32 m_LiveBlocks = 0;
        /// @brief The free runs, ascending by offset, never adjacent.
        vector<Range> m_Free;
        /// @brief Ranges awaiting reclaim, one bucket per frame-in-flight.
        vector<vector<Range>> m_PendingRelease;
    };
}
