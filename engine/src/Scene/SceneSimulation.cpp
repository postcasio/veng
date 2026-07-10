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
        m_Started = true;
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
        UpdatePhase(scene, SceneSystem::Phase::Sim, delta, context);
        UpdatePhase(scene, SceneSystem::Phase::View, delta, context);
    }

    void SceneSimulation::UpdatePhase(Scene& scene, const SceneSystem::Phase phase, const f32 delta,
                                      const SystemContext& context)
    {
        for (const Unique<SceneSystem>& system : m_Systems)
        {
            if (system->GetPhase() == phase)
            {
                system->OnUpdate(scene, delta, context);
            }
        }
    }

    void SceneSimulation::Stop(Scene& scene, const SystemContext& context)
    {
        m_Started = false;
        for (const Unique<SceneSystem>& system : m_Systems)
        {
            system->OnStop(scene, context);
        }
    }
}
