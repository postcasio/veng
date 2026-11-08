#include "MaterialArena.h"

#include <algorithm>

#include <Veng/Assert.h>

namespace Veng::Renderer
{
    void MaterialArena::Init(u32 arenaBytes, u32 framesInFlight)
    {
        m_ArenaBytes = arenaBytes - (arenaBytes % GranuleBytes);
        m_LiveBytes = 0;
        m_LiveBlocks = 0;
        m_Free.clear();
        if (m_ArenaBytes > 0)
        {
            m_Free.push_back(Range{.Offset = 0, .Bytes = m_ArenaBytes});
        }
        m_PendingRelease.assign(framesInFlight, {});
    }

    u32 MaterialArena::OccupiedBytes(u32 bytes)
    {
        const u32 granules = std::max(1u, (bytes + GranuleBytes - 1) / GranuleBytes);
        return granules * GranuleBytes;
    }

    optional<u32> MaterialArena::Allocate(u32 bytes)
    {
        const u32 wanted = OccupiedBytes(bytes);
        for (usize i = 0; i < m_Free.size(); ++i)
        {
            Range& run = m_Free[i];
            if (run.Bytes < wanted)
            {
                continue;
            }
            const u32 offset = run.Offset;
            if (run.Bytes == wanted)
            {
                m_Free.erase(m_Free.begin() + static_cast<isize>(i));
            }
            else
            {
                run.Offset += wanted;
                run.Bytes -= wanted;
            }
            m_LiveBytes += wanted;
            ++m_LiveBlocks;
            return offset;
        }
        return std::nullopt;
    }

    void MaterialArena::ReleaseDeferred(u32 offset, u32 bytes, u32 currentFrameInFlight)
    {
        VE_ASSERT(currentFrameInFlight < m_PendingRelease.size(),
                  "MaterialArena::ReleaseDeferred: frame-in-flight {} is outside the {} buckets",
                  currentFrameInFlight, m_PendingRelease.size());
        VE_ASSERT(bytes <= m_LiveBytes && m_LiveBlocks > 0,
                  "MaterialArena::ReleaseDeferred: releasing {} bytes at offset {} against {} live "
                  "bytes in {} blocks",
                  bytes, offset, m_LiveBytes, m_LiveBlocks);
        m_LiveBytes -= bytes;
        --m_LiveBlocks;
        m_PendingRelease[currentFrameInFlight].push_back(Range{.Offset = offset, .Bytes = bytes});
    }

    void MaterialArena::OnFrameAcquired(u32 frameInFlight)
    {
        VE_ASSERT(frameInFlight < m_PendingRelease.size(),
                  "MaterialArena::OnFrameAcquired: frame-in-flight {} is outside the {} buckets",
                  frameInFlight, m_PendingRelease.size());
        for (const Range& range : m_PendingRelease[frameInFlight])
        {
            Insert(range);
        }
        m_PendingRelease[frameInFlight].clear();
    }

    u32 MaterialArena::GetFreeBytes() const
    {
        u32 total = 0;
        for (const Range& run : m_Free)
        {
            total += run.Bytes;
        }
        return total;
    }

    u32 MaterialArena::GetLargestFreeRun() const
    {
        u32 largest = 0;
        for (const Range& run : m_Free)
        {
            largest = std::max(largest, run.Bytes);
        }
        return largest;
    }

    void MaterialArena::Insert(Range range)
    {
        const auto at = std::ranges::lower_bound(m_Free, range.Offset, {}, &Range::Offset);

        // Merge into the run ending where this one starts, then absorb the run starting where it
        // now ends, so the free list stays a set of maximal, non-adjacent runs after every reclaim.
        auto inserted = m_Free.insert(at, range);
        if (inserted != m_Free.begin())
        {
            const auto before = std::prev(inserted);
            if (before->Offset + before->Bytes == inserted->Offset)
            {
                before->Bytes += inserted->Bytes;
                m_Free.erase(inserted);
                inserted = before;
            }
        }
        const auto after = std::next(inserted);
        if (after != m_Free.end() && inserted->Offset + inserted->Bytes == after->Offset)
        {
            inserted->Bytes += after->Bytes;
            m_Free.erase(after);
        }
    }
}
