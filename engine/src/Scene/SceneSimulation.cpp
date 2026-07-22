#include <Veng/Scene/SceneSimulation.h>

#include <Veng/Diagnostics/Profiler.h>
#include <Veng/Scene/SystemRegistry.h>

namespace Veng
{
    namespace
    {
        // Interns a system's registered name against the active profiler once, at construction. The
        // per-system tick scope reuses the id, so the hot path never re-resolves the name (which
        // returns a string by value). Zero when no profiler is installed or under VE_PROFILE=OFF.
        Diagnostics::NameId InternSystemName(string_view name)
        {
            Diagnostics::Profiler* profiler = Diagnostics::GetActiveProfiler();
            return profiler != nullptr ? profiler->InternName(name) : 0;
        }
    }

    SceneSimulation::SceneSimulation(const SystemRegistry& registry)
    {
        const vector<SystemEntry>& entries = registry.Entries();
        m_Systems.reserve(entries.size());
        m_SystemProfileNames.reserve(entries.size());
        for (const SystemEntry& entry : entries)
        {
            m_Systems.emplace_back(entry.Factory());
            m_SystemProfileNames.push_back(InternSystemName(entry.Name));
        }
    }

    SceneSimulation::SceneSimulation(const SystemRegistry& registry,
                                     const vector<SystemId>& systemIds)
    {
        m_Systems.reserve(systemIds.size());
        m_SystemProfileNames.reserve(systemIds.size());
        for (const SystemId id : systemIds)
        {
            Unique<SceneSystem> system = registry.Instantiate(id);
            if (system == nullptr)
            {
                continue;
            }
            m_Systems.emplace_back(std::move(system));

            // Resolve the system's name against the catalog for the interned profile name; both
            // constructors have the name in hand, and neither used to keep it.
            string_view name;
            for (const SystemEntry& entry : registry.Entries())
            {
                if (entry.Id == id)
                {
                    name = entry.Name;
                    break;
                }
            }
            m_SystemProfileNames.push_back(InternSystemName(name));
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
        // The one deliberate high-cardinality instrumentation site: the systems are the phases here,
        // and their per-frame breakdown is the reason to profile a simulation. Each scope names the
        // system through the id interned once at construction.
        for (usize i = 0; i < m_Systems.size(); ++i)
        {
            const Unique<SceneSystem>& system = m_Systems[i];
            if (system->GetPhase() == phase)
            {
                VE_PROFILE_SCOPE_ID(m_SystemProfileNames[i]);
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
