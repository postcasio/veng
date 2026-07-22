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
        /// and the interpolation alpha.
        /// @param scene    The scene the systems operate over.
        /// @param phase    The phase whose systems run.
        /// @param delta    Time in seconds forwarded to each system's OnUpdate.
        /// @param context  Per-tick services forwarded to each system.
        void UpdatePhase(Scene& scene, SceneSystem::Phase phase, f32 delta,
                         const SystemContext& context);

        /// @brief Calls OnStop on each system, in registration order.
        /// @param scene    The scene the systems operate over.
        /// @param context  Per-tick services forwarded to each system.
        void Stop(Scene& scene, const SystemContext& context);

        /// @brief Returns true when no systems were registered.
        [[nodiscard]] bool IsEmpty() const { return m_Systems.empty(); }

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
        /// @brief The instantiated systems, in registration (run) order.
        vector<Unique<SceneSystem>> m_Systems;

        /// @brief Each system's registered name, interned once at construction, parallel to m_Systems.
        ///
        /// The catalog knows every system's name; interning it here (never per frame — SystemNameOf
        /// returns a string by value) gives the per-system tick scope a stable id with no per-frame
        /// allocation. Zero when no profiler was installed at construction. Empty under VE_PROFILE=OFF
        /// carries no cost; the per-system scope compiles out there.
        vector<Diagnostics::NameId> m_SystemProfileNames;

        /// @brief Whether the engine skips this simulation's per-frame tick (see SetPaused).
        bool m_Paused = false;

        /// @brief Whether Start has run without a matching Stop (see IsStarted).
        bool m_Started = false;
    };
}
