#include <Veng/Scene/SceneSimulation.h>

#include <Veng/Scene/SystemRegistry.h>

namespace Veng
{
    SceneSimulation::SceneSimulation(const SystemRegistry& registry)
        : m_Systems(registry.Instantiate())
    {
    }

    void SceneSimulation::Start(Scene& scene, const SystemContext& context)
    {
        for (const Unique<SceneSystem>& system : m_Systems)
        {
            system->OnStart(scene, context);
        }
    }

    void SceneSimulation::Update(Scene& scene, f32 delta, const SystemContext& context)
    {
        for (const Unique<SceneSystem>& system : m_Systems)
        {
            system->OnUpdate(scene, delta, context);
        }
    }

    void SceneSimulation::Stop(Scene& scene, const SystemContext& context)
    {
        for (const Unique<SceneSystem>& system : m_Systems)
        {
            system->OnStop(scene, context);
        }
    }
}
