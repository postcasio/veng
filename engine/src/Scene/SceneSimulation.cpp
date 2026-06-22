#include <Veng/Scene/SceneSimulation.h>

#include <Veng/Scene/SystemRegistry.h>

namespace Veng
{
    SceneSimulation::SceneSimulation(const SystemRegistry& registry)
        : m_Systems(registry.Instantiate())
    {
    }

    SceneSimulation::SceneSimulation(const SystemRegistry& registry,
                                     const vector<SystemId>& systemIds)
    {
        m_Systems.reserve(systemIds.size());
        for (const SystemId id : systemIds)
        {
            Unique<SceneSystem> system = registry.Instantiate(id);
            if (system != nullptr)
            {
                m_Systems.emplace_back(std::move(system));
            }
        }
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
        // Two partitioned passes over the registered systems: the deterministic Sim
        // phase finishes before any View system derives presentation from it, so a
        // camera rig reads pawn state the movement system already finalized this tick.
        for (const Unique<SceneSystem>& system : m_Systems)
        {
            if (system->GetPhase() == SceneSystem::Phase::Sim)
            {
                system->OnUpdate(scene, delta, context);
            }
        }
        for (const Unique<SceneSystem>& system : m_Systems)
        {
            if (system->GetPhase() == SceneSystem::Phase::View)
            {
                system->OnUpdate(scene, delta, context);
            }
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
