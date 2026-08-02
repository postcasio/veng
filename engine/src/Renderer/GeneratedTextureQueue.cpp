#include "GeneratedTextureQueue.h"

#include <algorithm>

#include <Veng/Assert.h>

namespace Veng::Renderer
{
    bool GeneratedTextureQueue::Add(const u64 key, const u32 tickCount, const i32 priority)
    {
        if (Contains(key))
        {
            return false;
        }
        m_Jobs.push_back({
            .Key = key,
            .Priority = priority,
            .Sequence = m_NextSequence++,
            .TickCount = std::max(1u, tickCount),
        });
        return true;
    }

    bool GeneratedTextureQueue::Remove(const u64 key)
    {
        const auto it = std::ranges::find(m_Jobs, key, &GeneratedTextureJobRecord::Key);
        if (it == m_Jobs.end())
        {
            return false;
        }
        m_Jobs.erase(it);
        return true;
    }

    bool GeneratedTextureQueue::SetPriority(const u64 key, const i32 priority)
    {
        GeneratedTextureJobRecord* record = FindMutable(key);
        if (record == nullptr)
        {
            return false;
        }
        record->Priority = priority;
        return true;
    }

    const GeneratedTextureJobRecord* GeneratedTextureQueue::Find(const u64 key) const
    {
        const auto it = std::ranges::find(m_Jobs, key, &GeneratedTextureJobRecord::Key);
        return it == m_Jobs.end() ? nullptr : &*it;
    }

    GeneratedTextureJobRecord* GeneratedTextureQueue::FindMutable(const u64 key)
    {
        const auto it = std::ranges::find(m_Jobs, key, &GeneratedTextureJobRecord::Key);
        return it == m_Jobs.end() ? nullptr : &*it;
    }

    u32 GeneratedTextureQueue::GetPendingCount() const
    {
        return static_cast<u32>(
            std::ranges::count_if(m_Jobs, [](const GeneratedTextureJobRecord& job)
                                  { return job.State != GeneratedTextureState::Resident; }));
    }

    u32 GeneratedTextureQueue::GetResidentCount() const
    {
        return static_cast<u32>(
            std::ranges::count_if(m_Jobs, [](const GeneratedTextureJobRecord& job)
                                  { return job.State == GeneratedTextureState::Resident; }));
    }

    optional<u64> GeneratedTextureQueue::NextKey() const
    {
        const GeneratedTextureJobRecord* best = nullptr;
        for (const GeneratedTextureJobRecord& job : m_Jobs)
        {
            if (job.State == GeneratedTextureState::Resident)
            {
                continue;
            }
            if (best == nullptr || job.Priority > best->Priority ||
                (job.Priority == best->Priority && job.Sequence < best->Sequence))
            {
                best = &job;
            }
        }
        return best == nullptr ? optional<u64>{} : optional<u64>{best->Key};
    }

    u32 GeneratedTextureQueue::Spend(const u32 budget, const function<void(u64, u32, u32)>& tick,
                                     const function<void(u64)>& complete)
    {
        u32 spent = 0;
        while (spent < budget)
        {
            const optional<u64> key = NextKey();
            if (!key)
            {
                break;
            }

            // The record is re-found after each callback: a caller's tick may not touch the queue,
            // but re-finding costs nothing and keeps the pointer from outliving a reallocation.
            GeneratedTextureJobRecord* record = FindMutable(*key);
            VE_ASSERT(record != nullptr, "generated-texture job {} vanished mid-spend", *key);

            const u32 tickIndex = record->TicksDone;
            const u32 tickCount = record->TickCount;
            record->State = GeneratedTextureState::Running;
            record->TicksDone = tickIndex + 1;
            spent++;

            tick(*key, tickIndex, tickCount);

            record = FindMutable(*key);
            VE_ASSERT(record != nullptr, "generated-texture job {} vanished in its tick", *key);
            if (record->TicksDone >= record->TickCount)
            {
                record->State = GeneratedTextureState::Resident;
                complete(*key);
            }
        }
        return spent;
    }
}
