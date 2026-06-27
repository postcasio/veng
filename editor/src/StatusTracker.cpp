#include "StatusTracker.h"

#include <algorithm>

namespace VengEditor
{
    using namespace Veng;

    StatusTracker::TaskId StatusTracker::Begin(string description)
    {
        // The first task after idle opens a new wave; reset the wave's progress counters.
        if (m_Active.empty())
        {
            m_Completed = 0;
            m_Total = 0;
        }
        ++m_Total;

        const TaskId id = m_NextId++;
        m_Active.push_back({.Id = id, .Description = std::move(description)});
        return id;
    }

    void StatusTracker::End(TaskId id)
    {
        const auto it =
            std::ranges::find_if(m_Active, [id](const Entry& entry) { return entry.Id == id; });
        if (it == m_Active.end())
        {
            return;
        }
        m_Active.erase(it);
        ++m_Completed;
    }

    StatusTracker::Snapshot StatusTracker::GetSnapshot() const
    {
        Snapshot snapshot;
        snapshot.CompletedInWave = m_Completed;
        snapshot.TotalInWave = m_Total;
        snapshot.Tasks.reserve(m_Active.size());
        for (const Entry& entry : m_Active)
        {
            snapshot.Tasks.push_back(entry.Description);
        }
        return snapshot;
    }
}
