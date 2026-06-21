#include <Veng/Scene/SystemRegistry.h>

namespace Veng
{
    vector<Unique<SceneSystem>> SystemRegistry::Instantiate() const
    {
        vector<Unique<SceneSystem>> systems;
        systems.reserve(m_Factories.size());
        for (const function<Unique<SceneSystem>()>& factory : m_Factories)
        {
            systems.emplace_back(factory());
        }
        return systems;
    }

    usize SystemRegistry::Count() const
    {
        return m_Factories.size();
    }
}
