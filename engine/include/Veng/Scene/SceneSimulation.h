#pragma once

#include <Veng/Veng.h>
#include <Veng/Diagnostics/TraceSink.h>
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
        /// The fixed-timestep drive splits this into UpdatePhase calls (N Sim steps, then one View);
        /// this single-call form runs one Sim step then one View for a caller with no accumulator.
        /// @param scene    The scene the systems operate over.
        /// @param delta    Time in seconds since the previous tick.
        /// @param context  Per-tick services forwarded to each system.
        void Update(Scene& scene, f32 delta, const SystemContext& context);

        /// @brief Calls OnUpdate on only the systems in the given phase, in registration order.
        ///
        /// The fixed-timestep drive runs the Sim phase once per fixed step (0..N times a frame) and
        /// the View phase once per frame, so it dispatches each phase separately rather than through
        /// Update. Sim carries the fixed step delta and the tick number; View carries the frame delta
        /// and the interpolation alpha. In the Sim phase each system runs only on the steps its
        /// SceneSystem::TickPolicy selects, keyed on the context's Tick, FirstStepThisFrame and
        /// LastStepThisFrame, and is handed the simulation time since it last ran.
        /// @param scene    The scene the systems operate over.
        /// @param phase    The phase whose systems run.
        /// @param delta    Time in seconds forwarded to each system's OnUpdate.
        /// @param context  Per-tick services forwarded to each system.
        void UpdatePhase(Scene& scene, SceneSystem::Phase phase, f32 delta,
                         const SystemContext& context);

        /// @brief Calls OnStop on each system, in registration order; a no-op when not started.
        ///
        /// Idempotent against Start: stopping a never-started or already-stopped simulation runs no
        /// OnStop, so a close path that stops the simulation before dropping its world is safe even
        /// when an earlier path already stopped it.
        /// @param scene    The scene the systems operate over.
        /// @param context  Per-tick services forwarded to each system.
        void Stop(Scene& scene, const SystemContext& context);

        /// @brief Returns true when no systems were registered.
        [[nodiscard]] bool IsEmpty() const { return m_Systems.empty(); }

        /// @brief Returns this simulation's instance of system @p T, or null when it runs none.
        ///
        /// The read-out seam for state a system keeps between ticks and a host needs to see (a
        /// playback clock, a tuning a caller pushes in): the instance is found by T's SystemId, so it
        /// is the one this simulation ticks, not a fresh copy. The first match wins when an id is
        /// named twice.
        /// @tparam T  The concrete SceneSystem subclass, declared with VE_SYSTEM.
        /// @return The running instance, or nullptr.
        template <class T>
        [[nodiscard]] T* FindSystem()
        {
            return static_cast<T*>(FindSystemById(SystemIdOf<T>()));
        }

        /// @brief Returns this simulation's instance of system @p T, or null when it runs none.
        /// @tparam T  The concrete SceneSystem subclass, declared with VE_SYSTEM.
        /// @return The running instance, or nullptr.
        template <class T>
        [[nodiscard]] const T* FindSystem() const
        {
            return static_cast<const T*>(FindSystemById(SystemIdOf<T>()));
        }

        /// @brief Pauses or resumes this simulation's per-frame tick.
        ///
        /// Paused, the engine's simulation drive-list skips this simulation's Update while still
        /// driving its scene's captures and view (registration, not run-state, gates those). The
        /// state is per-simulation, so one scene can pause while another keeps ticking. Start/Stop
        /// leave the pause state untouched.
        /// @param paused  True to skip ticking, false to resume.
        void SetPaused(bool paused) { m_Paused = paused; }

        /// @brief Returns whether this simulation's tick is paused.
        [[nodiscard]] bool IsPaused() const { return m_Paused; }

        /// @brief Returns whether Start has run and Stop has not, so the engine may tick this simulation.
        ///
        /// The engine's drive-list ticks a registered simulation only while it is started and not
        /// paused; Start sets this, Stop clears it. A simulation registered but never started is
        /// not auto-ticked.
        [[nodiscard]] bool IsStarted() const { return m_Started; }

    private:
        /// @brief Returns the first held system whose SystemId is @p id, or nullptr.
        /// @param id  The SystemId to find.
        [[nodiscard]] SceneSystem* FindSystemById(SystemId id) const;

        /// @brief The instantiated systems, in registration (run) order.
        vector<Unique<SceneSystem>> m_Systems;

        /// @brief Each system's SystemId, parallel to m_Systems.
        vector<SystemId> m_SystemIds;

        /// @brief Each system's registered name, interned once at construction, parallel to m_Systems.
        ///
        /// The catalog knows every system's name; interning it here (never per frame — SystemNameOf
        /// returns a string by value) gives the per-system scopes — each tick, OnStart and OnStop —
        /// a stable id with no per-frame allocation. Zero when no profiler was installed at
        /// construction. Empty under VE_PROFILE=OFF carries no cost; the scopes compile out there.
        vector<Diagnostics::NameId> m_SystemProfileNames;

        /// @brief Live Sim steps each frame-keyed system has let pass since it last ran, parallel to
        /// m_Systems.
        ///
        /// The multiple of the step delta a FirstStepOfFrame or LastStepOfFrame system is handed,
        /// counting the step it runs on. A reconciliation replay neither counts nor runs them.
        vector<u32> m_StepsSinceRun;

        /// @brief Whether the engine skips this simulation's per-frame tick (see SetPaused).
        bool m_Paused = false;

        /// @brief Whether Start has run without a matching Stop (see IsStarted).
        bool m_Started = false;
    };
}
