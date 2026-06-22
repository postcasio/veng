#include <Veng/Scene/SystemRegistry.h>

#include <algorithm>

namespace Veng
{
    Unique<SceneSystem> SystemRegistry::Instantiate(SystemId id) const
    {
        const auto it = std::ranges::find(m_Entries, id, &SystemEntry::Id);
        if (it == m_Entries.end())
        {
            return nullptr;
        }
        return it->Factory();
    }

    vector<Unique<SceneSystem>> SystemRegistry::Instantiate() const
    {
        vector<Unique<SceneSystem>> systems;
        systems.reserve(m_Entries.size());
        for (const SystemEntry& entry : m_Entries)
        {
            systems.emplace_back(entry.Factory());
        }
        return systems;
    }

    usize SystemRegistry::Count() const
    {
        return m_Entries.size();
    }
}
