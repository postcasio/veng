#include <Veng/WorldRunner.h>

#include <Veng/Assert.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Diagnostics/Profiler.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/InputMappingSystem.h>
#include <Veng/Scene/PresentationScope.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneSimulation.h>
#include <Veng/Time.h>

#include <algorithm>
#include <chrono>
#include <string>
#include <utility>

namespace Veng
{
    // ---- WorldPauseScope ---------------------------------------------------------------------------

    WorldPauseScope::WorldPauseScope(WorldRunner& runner, const WorldInstanceId world)
        : m_Runner(&runner), m_World(world)
    {
    }

    WorldPauseScope::WorldPauseScope(WorldPauseScope&& other) noexcept
        : m_Runner(other.m_Runner), m_World(other.m_World)
    {
        other.m_Runner = nullptr;
    }

    WorldPauseScope& WorldPauseScope::operator=(WorldPauseScope&& other) noexcept
    {
        if (this != &other)
        {
            Release();
            m_Runner = other.m_Runner;
            m_World = other.m_World;
            other.m_Runner = nullptr;
        }
        return *this;
    }

    WorldPauseScope::~WorldPauseScope()
    {
        Release();
    }

    void WorldPauseScope::Release()
    {
        if (m_Runner != nullptr)
        {
            m_Runner->ReleasePause(m_World);
            m_Runner = nullptr;
        }
    }

    // ---- World -------------------------------------------------------------------------------------

    bool World::IsPaused() const
    {
        return LiveScene != nullptr && LiveScene->IsSimulationPaused();
    }

    // ---- WorldRunner -------------------------------------------------------------------------------

    WorldRunner::WorldRunner(const WorldRunnerInfo& info)
        : m_Types(info.Types), m_Systems(info.Systems), m_Presentation(info.Presentation),
          m_Assets(info.Assets)
    {
        VE_ASSERT(m_Types != nullptr, "WorldRunner requires a TypeRegistry");
        VE_ASSERT(m_Systems != nullptr, "WorldRunner requires a SystemRegistry");
        VE_ASSERT(m_Presentation != nullptr, "WorldRunner requires a PresentationScopes registry");
    }

    WorldRunner::~WorldRunner() = default;

    WorldInstanceId WorldRunner::MintId()
    {
        return WorldInstanceId{.Value = m_NextId++};
    }

    Unique<World> WorldRunner::CreateWorld(const WorldOpenInfo& info)
    {
        auto world = CreateUnique<World>();
        world->Id = MintId();
        world->Clock = SimClock(SimClockInfo{
            .TickRate = info.SimTickRate,
            .MaxTicksPerFrame = info.MaxTicksPerFrame,
            .MaxSimMillisecondsPerFrame = info.MaxSimMillisecondsPerFrame,
        });
        if (Diagnostics::Profiler* profiler = Diagnostics::GetActiveProfiler(); profiler != nullptr)
        {
            // Named with the world's identity so several worlds read side by side rather than summed.
            const string prefix = "World " + std::to_string(world->Id.Value);
            world->SimScopeName = profiler->InternName(prefix + " Sim");
            world->ViewScopeName = profiler->InternName(prefix + " View");
        }
        return world;
    }

    WorldInstanceId WorldRunner::OpenWorld(const WorldOpenInfo& info)
    {
        VE_PROFILE_SCOPE("World/Open");
        Unique<World> world = CreateWorld(info);

        if (info.Source.IsLoaded())
        {
            VE_ASSERT(m_Assets != nullptr, "WorldRunner: opening a cooked-level world needs an "
                                           "AssetManager");
            VE_PROFILE_SCOPE("World/Load");
            LevelInstance instance = info.Source.Get()->LoadInto(*m_Assets, *m_Systems, info.Load);
            world->OwnedScene = std::move(instance.World);
            world->Pending = std::move(instance.Pending);
        }
        else
        {
            world->OwnedScene = Scene::Create(*m_Types);
            if (info.Systems.has_value())
            {
                world->OwnedScene->SetSimulation(
                    CreateUnique<SceneSimulation>(*m_Systems, *info.Systems));
            }
        }
        return AdoptWorld(std::move(world), info);
    }

    WorldInstanceId WorldRunner::OpenWorld(const WorldOpenInfo& info, Unique<Scene> scene)
    {
        VE_PROFILE_SCOPE("World/Open");
        VE_ASSERT(!info.Source.IsValid(),
                  "WorldRunner::OpenWorld: a world opened over a scene spawns no level");
        VE_ASSERT(scene != nullptr, "WorldRunner::OpenWorld: no scene to open a world over");
        Unique<World> world = CreateWorld(info);
        world->OwnedScene = std::move(scene);
        if (info.Systems.has_value())
        {
            world->OwnedScene->SetSimulation(
                CreateUnique<SceneSimulation>(*m_Systems, *info.Systems));
        }
        return AdoptWorld(std::move(world), info);
    }

    WorldInstanceId WorldRunner::AdoptWorld(Unique<World> world, const WorldOpenInfo& info)
    {
        world->LiveScene = world->OwnedScene.get();
        // Before the load hook and the start, so every context built for the scene finds its scope.
        world->LiveScene->SetPresentationScope(m_Presentation->Open());

        const WorldInstanceId id = world->Id;
        Scene& scene = *world->LiveScene;
        ResidencyBatch& pending = world->Pending;
        m_Worlds.push_back(std::move(world));

        if (info.OnLoaded)
        {
            VE_PROFILE_SCOPE("World/OnLoaded");
            info.OnLoaded(id, scene, pending);
        }

        if (info.StartSimulation && scene.GetSimulation() != nullptr)
        {
            VE_ASSERT(m_ContextFactory != nullptr,
                      "WorldRunner: StartSimulation needs a context factory");
            VE_PROFILE_SCOPE("World/Start");
            scene.StartSimulation(BuildContext(SystemContextRequest{
                .World = id, .Scene = scene, .Phase = SystemContextPhase::Start}));
        }

        return id;
    }

    void WorldRunner::StartWorld(const WorldInstanceId world)
    {
        World* resolved = ResolveWorld(world);
        VE_ASSERT(resolved != nullptr, "WorldRunner::StartWorld: world {} is not open",
                  world.Value);
        Scene& scene = resolved->GetScene();
        const SceneSimulation* sim = scene.GetSimulation();
        if (sim == nullptr)
        {
            return;
        }
        VE_ASSERT(!sim->IsStarted(), "WorldRunner::StartWorld: world {} is already started",
                  world.Value);
        VE_PROFILE_SCOPE("World/Start");
        scene.StartSimulation(BuildContext(SystemContextRequest{
            .World = world, .Scene = scene, .Phase = SystemContextPhase::Start}));
    }

    void WorldRunner::StopWorld(const WorldInstanceId world)
    {
        // Mid-walk a stop could land between a world's Sim steps, running OnStop under a system still
        // inside its own update; the callers (a drained request, a net connect) run between ticks.
        VE_ASSERT(!m_Ticking, "WorldRunner::StopWorld is not legal inside Tick");
        World* resolved = ResolveWorld(world);
        if (resolved == nullptr)
        {
            return;
        }
        Scene& scene = resolved->GetScene();
        if (const SceneSimulation* sim = scene.GetSimulation(); sim != nullptr && sim->IsStarted())
        {
            VE_ASSERT(m_ContextFactory != nullptr,
                      "WorldRunner::StopWorld: stopping a started world needs a context factory");
        }
        StopScene(world, scene);
    }

    void WorldRunner::StopScene(const WorldInstanceId world, Scene& scene)
    {
        // A destructor has no SystemContext to supply, so this stop is the one place OnStop runs; a
        // system releasing an engine-owned resource there depends on it. Stopping is idempotent, so a
        // caller that already stopped takes a harmless no-op here.
        const SceneSimulation* sim = scene.GetSimulation();
        if (sim == nullptr || !sim->IsStarted() || !m_ContextFactory)
        {
            return;
        }
        VE_PROFILE_SCOPE("World/Stop");
        scene.StopSimulation(BuildContext(SystemContextRequest{
            .World = world, .Scene = scene, .Phase = SystemContextPhase::Stop}));
    }

    void WorldRunner::SetContextFactory(SystemContextFactory factory)
    {
        m_ContextFactory = std::move(factory);
    }

    SystemContext WorldRunner::BuildContext(const SystemContextRequest& request) const
    {
        VE_ASSERT(m_ContextFactory != nullptr, "WorldRunner::BuildContext: no context factory");
        VE_ASSERT(request.World.IsValid(), "WorldRunner::BuildContext: the context names no world");
        VE_ASSERT(request.Scene.GetPresentationScope() != nullptr,
                  "WorldRunner::BuildContext: world {}'s scene carries no presentation scope",
                  request.World.Value);
        return m_ContextFactory(request);
    }

    WorldInstanceId WorldRunner::FindWorld(const Scene& scene) const
    {
        const auto it = std::ranges::find_if(m_Worlds, [&scene](const Unique<World>& w)
                                             { return &w->GetScene() == &scene; });
        return it != m_Worlds.end() ? (*it)->Id : WorldInstanceId{};
    }

    void WorldRunner::ReplaySimStep(Scene& scene, const u64 tick)
    {
        const WorldInstanceId world = FindWorld(scene);
        VE_ASSERT(world.IsValid(), "WorldRunner::ReplaySimStep: the scene is no world's");
        const f32 simDelta = 1.0f / static_cast<f32>(ResolveWorld(world)->Clock.GetTickRate());
        scene.TickSimulationPhase(
            SceneSystem::Phase::Sim, simDelta,
            BuildContext(SystemContextRequest{.World = world,
                                              .Scene = scene,
                                              .Phase = SystemContextPhase::Replay,
                                              .Tick = tick}));
    }

    void WorldRunner::SetSceneRetiringHook(function<void(const Scene&)> hook)
    {
        m_SceneRetiringHook = std::move(hook);
    }

    void WorldRunner::SetWorldClosedHook(function<void(WorldInstanceId)> hook)
    {
        m_WorldClosedHook = std::move(hook);
    }

    void WorldRunner::CloseAllWorlds()
    {
        VE_ASSERT(!m_Ticking, "WorldRunner::CloseAllWorlds is not legal inside Tick");
        // m_Worlds is in ascending id order, so its back is the newest world. Re-read after each close,
        // since an OnStop may open a world (which is then the newest) or close another.
        while (!m_Worlds.empty())
        {
            CloseWorldNow(m_Worlds.back()->Id);
        }
    }

    void WorldRunner::CloseWorld(const WorldInstanceId world)
    {
        // Inside a tick the walk owns m_Worlds, so the close is queued rather than erasing under it.
        // Queuing rather than refusing is what lets a world-managing system act on its own decision
        // — reap a finished match, reload a level, tear down the session it came from — at the point
        // it makes it, instead of publishing a marker for the application to drive back through.
        // The world is skipped for the rest of the walk (closest to the immediate close it replaces,
        // and what keeps a closing hook's capture from going stale) and stopped at the drain.
        if (m_Ticking)
        {
            if (ResolveWorld(world) != nullptr && !IsCloseQueued(world))
            {
                m_PendingCloses.push_back(world);
            }
            return;
        }
        CloseWorldNow(world);
    }

    bool WorldRunner::IsCloseQueued(const WorldInstanceId world) const
    {
        return std::ranges::find(m_PendingCloses, world) != m_PendingCloses.end();
    }

    void WorldRunner::DrainPendingCloses()
    {
        // By index over a queue that may grow as it drains: a system's OnStop may itself close a
        // world, which appends here and is honoured in turn. Each id is re-resolved by CloseWorldNow
        // rather than held as an iterator or an index into m_Worlds, both of which the erases move.
        for (usize i = 0; i < m_PendingCloses.size(); ++i)
        {
            CloseWorldNow(m_PendingCloses[i]);
        }
        m_PendingCloses.clear();
    }

    void WorldRunner::CloseWorldNow(const WorldInstanceId world)
    {
        VE_PROFILE_SCOPE("World/Close");
        const auto it = std::ranges::find_if(m_Worlds, [world](const Unique<World>& w)
                                             { return w->Id == world; });
        if (it == m_Worlds.end())
        {
            return;
        }

        // End-play before teardown: each system's OnStop runs while its scene is still live.
        Scene& scene = (*it)->GetScene();
        StopScene(world, scene);

        if (m_SceneRetiringHook)
        {
            m_SceneRetiringHook(scene);
        }

        // Erase by id rather than through the iterator found above: a system's OnStop may close a
        // world itself, and an immediate close of another world moves this one's slot out from under
        // a held iterator (one issued inside a tick is queued instead, and cannot).
        {
            VE_PROFILE_SCOPE("World/Destroy");
            std::erase_if(m_Worlds, [world](const Unique<World>& w) { return w->Id == world; });
        }

        if (m_WorldClosedHook)
        {
            m_WorldClosedHook(world);
        }
    }

    const World* WorldRunner::ResolveWorld(const WorldInstanceId world) const
    {
        if (!world.IsValid())
        {
            return nullptr;
        }
        const auto it = std::ranges::find_if(m_Worlds, [world](const Unique<World>& w)
                                             { return w->Id == world; });
        return it != m_Worlds.end() ? it->get() : nullptr;
    }

    World* WorldRunner::ResolveWorld(const WorldInstanceId world)
    {
        return const_cast<World*>(std::as_const(*this).ResolveWorld(world));
    }

    optional<CameraView> WorldRunner::ResolveCameraView(const WorldInstanceId world,
                                                        const Entity viewer, const f32 aspect) const
    {
        const World* resolved = ResolveWorld(world);
        if (resolved == nullptr)
        {
            return std::nullopt;
        }
        const Scene& scene = resolved->GetScene();
        if (viewer == Entity::Null)
        {
            return ResolvePrimaryCameraView(scene, aspect);
        }
        return Veng::ResolveCameraView(scene, viewer, aspect);
    }

    f32 WorldRunner::ResolveAlpha(const WorldInstanceId world) const
    {
        const World* resolved = ResolveWorld(world);
        return resolved != nullptr ? resolved->LastAlpha : 0.0f;
    }

    Scene& WorldRunner::InstallScene(const WorldInstanceId world, Unique<Scene> scene)
    {
        World* resolved = ResolveWorld(world);
        VE_ASSERT(resolved != nullptr, "WorldRunner::InstallScene: unminted world");
        VE_ASSERT(scene != nullptr, "WorldRunner::InstallScene: no scene to install");
        scene->SetPresentationScope(m_Presentation->Open());
        if (resolved->OwnedScene != nullptr)
        {
            StopScene(world, *resolved->OwnedScene);
            if (m_SceneRetiringHook)
            {
                m_SceneRetiringHook(*resolved->OwnedScene);
            }
        }
        // The pause lives on the simulation, so it is carried across: a holder of the replaced scene's
        // pause (an overlay over a world a client join replaces) still holds it, and releases here.
        SceneSimulation* const previous =
            resolved->OwnedScene != nullptr ? resolved->OwnedScene->GetSimulation() : nullptr;
        SceneSimulation* const next = scene->GetSimulation();
        if (previous != nullptr && next != nullptr)
        {
            next->AdoptPause(*previous);
        }
        resolved->OwnedScene = std::move(scene);
        resolved->LiveScene = resolved->OwnedScene.get();
        return *resolved->LiveScene;
    }

    WorldTickResult WorldRunner::Tick(const WorldTickInfo& info)
    {
        VE_PROFILE_SCOPE("WorldRunner/Tick");

        VE_ASSERT(!m_Ticking, "WorldRunner::Tick is not reentrant");
        m_Ticking = true;

        WorldTickResult result;
        // A driven frame clock reads a constant wall time, which leaves every world's budget unspent.
        const bool driven = Time::IsDriven();
        const auto budgetClock = [driven]
        {
            return driven ? 0.0
                          : std::chrono::duration<f64>(
                                std::chrono::steady_clock::now().time_since_epoch())
                                .count();
        };
        // By index over the count captured at entry, holding the heap World rather than a reference
        // into the vector: a world opened from inside a system's update appends to m_Worlds, which
        // can reallocate and move every Unique slot — the World it points at does not move. The
        // fixed count leaves the appended world's first tick to the next frame.
        const usize count = m_Worlds.size();
        for (usize i = 0; i < count; ++i)
        {
            World* const world = m_Worlds[i].get();
            if (IsCloseQueued(world->Id))
            {
                // Closed earlier in this walk: it takes no phase at all this frame, and its clock is
                // left alone — it is about to be stopped and dropped at the drain.
                continue;
            }

            Scene& scene = world->GetScene();
            const SceneSimulation* sim = scene.GetSimulation();
            const bool active = sim != nullptr && sim->IsStarted() && !sim->IsPaused();
            if (!active)
            {
                // A paused or unstarted world drops its accumulator so resuming chases no backlog. It
                // runs no step either, so it clears its frame edges as a stepless frame does below.
                world->Clock.Reset();
                world->LastAlpha = 0.0f;
                ResetFrameActionEdges(scene);
                continue;
            }
            result.AnyActive = true;

            const f32 scale = info.SimScale ? info.SimScale(world->Id) : 1.0f;
            SimStep step;
            {
                VE_PROFILE_SCOPE_ID(world->SimScopeName);
                step = world->Clock.Run(
                    info.Delta * scale,
                    [&](const SimStepInfo& simStep)
                    {
                        if (info.BeforeSimStep)
                        {
                            info.BeforeSimStep(world->Id, scene, simStep.Tick);
                        }
                        scene.TickSimulationPhase(
                            SceneSystem::Phase::Sim, simStep.Delta,
                            BuildContext(SystemContextRequest{.World = world->Id,
                                                              .Scene = scene,
                                                              .Phase = SystemContextPhase::Sim,
                                                              .Tick = simStep.Tick,
                                                              .FirstStep = simStep.First,
                                                              .LastStep = simStep.Last}),
                            simStep.RecordsHistory);
                        if (info.AfterSimStep)
                        {
                            info.AfterSimStep(world->Id, scene, simStep.Tick);
                        }
                        // A system closed this world from the step it just ran: no further step.
                        return !IsCloseQueued(world->Id);
                    },
                    budgetClock);
                // The step counter distinguishes a heavy simulation from a frame that spiralled into
                // multiple fixed-step catch-up steps; the dropped time is how far it dilated.
                VE_PROFILE_COUNTER("WorldRunner/SimSteps", static_cast<f64>(step.Steps));
                VE_PROFILE_COUNTER("WorldRunner/DroppedMs",
                                   static_cast<f64>(step.DroppedSeconds) * 1000.0);
            }
            world->LastAlpha = step.Alpha;
            if (step.Steps > 0)
            {
                result.AnyTicked = true;
            }
            else
            {
                // No step reset the frame edges, so View would read the last frame's.
                ResetFrameActionEdges(scene);
            }

            if (info.RunViewPhase && !IsCloseQueued(world->Id))
            {
                VE_PROFILE_SCOPE_ID(world->ViewScopeName);
                scene.TickSimulationPhase(
                    SceneSystem::Phase::View, info.Delta,
                    BuildContext(SystemContextRequest{.World = world->Id,
                                                      .Scene = scene,
                                                      .Phase = SystemContextPhase::View,
                                                      .Tick = world->Clock.GetTick(),
                                                      .Alpha = step.Alpha}));
            }
        }

        // The drain is itself part of the tick: IsTicking stays true through it, so a close issued
        // from a system's OnStop queues behind the one being drained rather than erasing mid-drain,
        // and a world opened from an OnStop simply first ticks next frame.
        DrainPendingCloses();
        m_Ticking = false;
        return result;
    }

    SceneSimulation* WorldRunner::ResolveSimulation(const WorldInstanceId world) const
    {
        const World* resolved = ResolveWorld(world);
        return resolved != nullptr ? resolved->GetScene().GetSimulation() : nullptr;
    }

    void WorldRunner::SetWorldPaused(const WorldInstanceId world, const bool paused)
    {
        if (SceneSimulation* sim = ResolveSimulation(world); sim != nullptr)
        {
            sim->SetPaused(paused);
        }
    }

    bool WorldRunner::IsWorldPaused(const WorldInstanceId world) const
    {
        const World* resolved = ResolveWorld(world);
        return resolved != nullptr && resolved->IsPaused();
    }

    void WorldRunner::ReleasePause(const WorldInstanceId world)
    {
        // Resolved at release time: the world may have closed, or had its scene replaced (which
        // carried the pause onto the installed simulation).
        if (SceneSimulation* sim = ResolveSimulation(world); sim != nullptr)
        {
            sim->ReleasePause();
        }
    }

    WorldPauseScope WorldRunner::PauseScope(const WorldInstanceId world)
    {
        SceneSimulation* sim = ResolveSimulation(world);
        if (sim == nullptr)
        {
            return WorldPauseScope{};
        }
        sim->AcquirePause();
        return WorldPauseScope(*this, world);
    }
}
