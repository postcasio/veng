#include "SphereTerrainResidency.h"

#include <algorithm>

#include <Veng/Assert.h>

namespace Veng::Renderer
{
    SphereTerrainResidency::SphereTerrainResidency(const u32 capacity, const u32 pinnedLevel,
                                                   const f64 radius)
        : m_Capacity(capacity), m_PinnedLevel(pinnedLevel), m_Radius(radius)
    {
        m_FreeLayers.reserve(capacity);
        for (u32 layer = capacity; layer > 0; layer--)
        {
            m_FreeLayers.push_back(layer - 1);
        }
    }

    void SphereTerrainResidency::BeginFrame(const u64 frame)
    {
        m_Frame = frame;
    }

    const SphereTerrainTileRecord* SphereTerrainResidency::Find(const CubeSphereTileKey key) const
    {
        const auto it = m_Tiles.find(key.Value);
        return it != m_Tiles.end() ? &it->second : nullptr;
    }

    bool SphereTerrainResidency::IsResident(const CubeSphereTileKey key) const
    {
        const SphereTerrainTileRecord* record = Find(key);
        return record != nullptr && record->State == SphereTerrainTileState::Resident;
    }

    void SphereTerrainResidency::MarkWanted(const CubeSphereTileKey key)
    {
        if (const auto it = m_Tiles.find(key.Value); it != m_Tiles.end())
        {
            SetLastWanted(it->second, m_Frame);
        }
    }

    bool SphereTerrainResidency::IsBucketed(const SphereTerrainTileRecord& record)
    {
        return record.State == SphereTerrainTileState::Resident && !record.Pinned &&
               record.Children == 0;
    }

    void SphereTerrainResidency::Unbucket(const SphereTerrainTileRecord& record)
    {
        if (!IsBucketed(record))
        {
            return;
        }
        const auto bucket = m_Buckets.find(record.LastWanted);
        VE_ASSERT(bucket != m_Buckets.end(), "sphere terrain: an evictable tile is in no bucket");
        bucket->second.erase(record.Key.Value);
        if (bucket->second.empty())
        {
            m_Buckets.erase(bucket);
        }
    }

    void SphereTerrainResidency::Bucket(const SphereTerrainTileRecord& record)
    {
        if (IsBucketed(record))
        {
            m_Buckets[record.LastWanted].insert(record.Key.Value);
        }
    }

    void SphereTerrainResidency::SetLastWanted(SphereTerrainTileRecord& record, const u64 frame)
    {
        if (record.LastWanted == frame)
        {
            return;
        }
        Unbucket(record);
        record.LastWanted = frame;
        Bucket(record);
    }

    void SphereTerrainResidency::AddChildren(SphereTerrainTileRecord& record, const i32 delta)
    {
        Unbucket(record);
        record.Children = static_cast<u8>(static_cast<i32>(record.Children) + delta);
        Bucket(record);
    }

    CubeSphereTileKey SphereTerrainResidency::FindEvictable() const
    {
        // Wanted this frame or the last is protected: renderers sharing a source mark in turn, so a
        // tile one marked last frame and another is about to mark this frame must survive between.
        // The oldest bucket holds the least recently wanted, so when it is protected, all are.
        if (m_Buckets.empty() || m_Buckets.begin()->first + 1 >= m_Frame)
        {
            return {};
        }
        return CubeSphereTileKey::FromValue(*m_Buckets.begin()->second.begin());
    }

    void SphereTerrainResidency::Drop(const CubeSphereTileKey key)
    {
        const auto it = m_Tiles.find(key.Value);
        VE_ASSERT(it != m_Tiles.end(), "sphere terrain: dropping a tile the pool does not hold");
        VE_ASSERT(it->second.Children == 0, "sphere terrain: dropping a tile with a held child");
        Unbucket(it->second);
        if (it->second.State == SphereTerrainTileState::Pending)
        {
            m_PendingCount--;
        }
        if (key.GetLevel() > 0)
        {
            if (const auto parent = m_Tiles.find(key.GetParent().Value); parent != m_Tiles.end())
            {
                AddChildren(parent->second, -1);
            }
        }
        m_FreeLayers.push_back(it->second.Layer);
        m_Tiles.erase(it);
    }

    SphereTerrainResidency::RequestResult
    SphereTerrainResidency::Request(const CubeSphereTileKey key)
    {
        if (const auto it = m_Tiles.find(key.Value); it != m_Tiles.end())
        {
            SetLastWanted(it->second, m_Frame);
            return {.Outcome = RequestOutcome::AlreadyHeld,
                    .Layer = it->second.Layer,
                    .Generation = it->second.Generation};
        }
        if (key.GetLevel() > 0 && !IsResident(key.GetParent()))
        {
            return {.Outcome = RequestOutcome::ParentMissing};
        }

        RequestResult result{.Outcome = RequestOutcome::Accepted};
        if (key.GetLevel() > 0)
        {
            // Wanting a child wants its parent, which must survive the eviction below.
            SetLastWanted(m_Tiles.at(key.GetParent().Value), m_Frame);
        }
        if (m_FreeLayers.empty())
        {
            const CubeSphereTileKey evict = FindEvictable();
            if (!evict.IsValid())
            {
                m_Deferred++;
                return {.Outcome = RequestOutcome::Deferred};
            }
            Drop(evict);
            m_Evictions++;
            result.Evicted = evict;
        }

        const u32 layer = m_FreeLayers.back();
        m_FreeLayers.pop_back();
        if (key.GetLevel() > 0)
        {
            AddChildren(m_Tiles.at(key.GetParent().Value), 1);
        }
        const u32 generation = m_NextGeneration++;
        m_Tiles.emplace(key.Value, SphereTerrainTileRecord{
                                       .Key = key,
                                       .Layer = layer,
                                       .State = SphereTerrainTileState::Pending,
                                       .Pinned = key.GetLevel() <= m_PinnedLevel,
                                       .Generation = generation,
                                       .LastWanted = m_Frame,
                                       .HasFacts = true,
                                       .Facts = MakeSphereTerrainTileFacts(key, m_Radius),
                                   });
        m_PendingCount++;
        result.Layer = layer;
        result.Generation = generation;
        return result;
    }

    bool SphereTerrainResidency::Complete(const CubeSphereTileKey key, const u32 generation)
    {
        const auto it = m_Tiles.find(key.Value);
        if (it == m_Tiles.end() || it->second.Generation != generation ||
            it->second.State != SphereTerrainTileState::Pending)
        {
            return false;
        }
        it->second.State = SphereTerrainTileState::Resident;
        m_PendingCount--;
        Bucket(it->second);
        return true;
    }

    bool SphereTerrainResidency::SetBound(const CubeSphereTileKey key, const u32 generation,
                                          const SphereTerrainTileBound& bound)
    {
        const auto it = m_Tiles.find(key.Value);
        if (it == m_Tiles.end() || it->second.Generation != generation)
        {
            return false;
        }
        it->second.Bound = bound;
        return true;
    }

    bool SphereTerrainResidency::SetTileWords(const CubeSphereTileKey key,
                                              const std::span<const u32> words)
    {
        const auto it = m_Tiles.find(key.Value);
        if (it == m_Tiles.end())
        {
            return false;
        }
        std::array<u32, MaxSphereTerrainTileWords>& held = it->second.Facts.Words;
        held = {};
        std::ranges::copy(words.first(std::min(words.size(), held.size())), held.begin());
        return true;
    }

    bool SphereTerrainResidency::Remove(const CubeSphereTileKey key)
    {
        if (!m_Tiles.contains(key.Value))
        {
            return false;
        }
        Drop(key);
        return true;
    }

    vector<CubeSphereTileKey> SphereTerrainResidency::CollectStalePending() const
    {
        vector<CubeSphereTileKey> stale;
        for (const auto& [value, record] : m_Tiles)
        {
            if (record.State == SphereTerrainTileState::Pending && !record.Pinned &&
                record.Children == 0 && record.LastWanted + 1 < m_Frame)
            {
                stale.push_back(record.Key);
            }
        }
        return stale;
    }

    SphereTerrainResidency::CapacityChange SphereTerrainResidency::SetCapacity(const u32 capacity)
    {
        VE_ASSERT(GetPendingCount() == 0,
                  "sphere terrain: the pool's capacity changes only with nothing pending");
        CapacityChange change;
        while (GetHeldCount() > capacity)
        {
            const CubeSphereTileKey evict = FindEvictable();
            if (!evict.IsValid())
            {
                break;
            }
            Drop(evict);
            m_Evictions++;
            change.Evicted.push_back(evict);
        }
        change.Capacity = std::max(capacity, GetHeldCount());

        // Compact: every tile above the new range takes the lowest free layer inside it.
        vector<bool> used(change.Capacity, false);
        for (const auto& [value, record] : m_Tiles)
        {
            if (record.Layer < change.Capacity)
            {
                used[record.Layer] = true;
            }
        }
        u32 nextFree = 0;
        for (auto& [value, record] : m_Tiles)
        {
            if (record.Layer < change.Capacity)
            {
                continue;
            }
            while (used[nextFree])
            {
                nextFree++;
            }
            change.Moves.emplace_back(record.Layer, nextFree);
            record.Layer = nextFree;
            used[nextFree] = true;
        }

        m_Capacity = change.Capacity;
        m_FreeLayers.clear();
        for (u32 layer = m_Capacity; layer > 0; layer--)
        {
            if (!used[layer - 1])
            {
                m_FreeLayers.push_back(layer - 1);
            }
        }
        return change;
    }

    u32 SphereTerrainResidency::GetResidentCount() const
    {
        return static_cast<u32>(std::ranges::count_if(
            m_Tiles, [](const auto& entry)
            { return entry.second.State == SphereTerrainTileState::Resident; }));
    }

    u32 SphereTerrainResidency::GetPinnedResidentCount() const
    {
        return static_cast<u32>(
            std::ranges::count_if(m_Tiles,
                                  [](const auto& entry)
                                  {
                                      return entry.second.Pinned &&
                                             entry.second.State == SphereTerrainTileState::Resident;
                                  }));
    }
}
