#pragma once

#include <Veng/Veng.h>
#include <Veng/Scene/SceneSystem.h>

namespace Veng
{
    class SystemRegistry;

    /// @brief Drives a set of SceneSystems over a Scene.
    ///
    /// The single simulation driver both the runtime app and the editor's Play mode
    /// own. Constructed either from an ordered SystemId set selecting catalog entries —
    /// it runs exactly those systems, in that order — or from a whole SystemRegistry as
    /// the "all registered" convenience. It instantiates its systems at construction and
    /// holds them, then Start/Update/Stop each across a play session, honoring the
    /// Sim/View phase split each tick.
    class SceneSimulation
    {
    public:
        /// @brief Instantiates every registered system and holds it for the session.
        ///
        /// The "all registered" convenience: builds one of each catalog entry in
        /// registration order. Used by tests and the no-level case.
        /// @param registry  Host-owned catalog whose entries produce the systems.
        explicit SceneSimulation(const SystemRegistry& registry);

        /// @brief Instantiates the named systems, in the given order, and holds them for the session.
        ///
        /// Resolves each SystemId against the catalog and builds the system it names, so
        /// the simulation runs exactly the named set in the named order. An id absent
        /// from the catalog is skipped.
        /// @param registry  Host-owned catalog the ids resolve against.
        /// @param systemIds The active ordered SystemId set.
        SceneSimulation(const SystemRegistry& registry, const vector<SystemId>& systemIds);

        /// @brief Calls OnStart on each system, in registration order.
        /// @param scene    The scene the systems operate over.
        /// @param context  Per-tick services forwarded to each system.
        void Start(Scene& scene, const SystemContext& context);

        /// @brief Calls OnUpdate on each system in two passes: all Sim systems, then all View systems.
        ///
        /// Within each phase, systems run in registration order. The two-pass split lets
        /// a View system (a camera rig) read the state the Sim systems finalized this
        /// tick; it is the whole scheduling mechanism — no dependency graph, no parallelism.
        /// @param scene    The scene the systems operate over.
        /// @param delta    Time in seconds since the previous tick.
        /// @param context  Per-tick services forwarded to each system.
        void Update(Scene& scene, f32 delta, const SystemContext& context);

        /// @brief Calls OnStop on each system, in registration order.
        /// @param scene    The scene the systems operate over.
        /// @param context  Per-tick services forwarded to each system.
        void Stop(Scene& scene, const SystemContext& context);

        /// @brief Returns true when no systems were registered.
        [[nodiscard]] bool IsEmpty() const { return m_Systems.empty(); }

    private:
        /// @brief The instantiated systems, in registration (run) order.
        vector<Unique<SceneSystem>> m_Systems;
    };
}
