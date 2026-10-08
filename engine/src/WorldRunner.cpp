#include <Veng/WorldRunner.h>

#include <Veng/Assert.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Diagnostics/Profiler.h>
#include <Veng/Renderer/CaptureSurface.h>
#include <Veng/Renderer/SceneCapture.h>
#include <Veng/Renderer/SceneCapturePool.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/InputMappingSystem.h>
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

    // ---- WorldRunner -------------------------------------------------------------------------------

    WorldRunner::WorldRunner(const WorldRunnerInfo& info)
        : m_Types(info.Types), m_Systems(info.Systems), m_Assets(info.Assets),
          m_Context(info.Context)
    {
        VE_ASSERT(m_Types != nullptr, "WorldRunner requires a TypeRegistry");
        VE_ASSERT(m_Systems != nullptr, "WorldRunner requires a SystemRegistry");
        if (m_Context != nullptr && m_Assets != nullptr)
        {
            m_CapturePool = CreateRef<Renderer::SceneCapturePool>();
        }
    }

    WorldRunner::~WorldRunner() = default;

    WorldInstanceId WorldRunner::MintId()
    {
        return WorldInstanceId{.Value = m_NextId++};
    }

    WorldInstanceId WorldRunner::OpenWorld(const WorldOpenInfo& info)
    {
        VE_PROFILE_SCOPE("World/Open");
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
        world->LiveScene = world->OwnedScene.get();

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

    void WorldRunner::SetContextFactory(SystemContextFactory factory)
    {
        m_ContextFactory = std::move(factory);
    }

    SystemContext WorldRunner::BuildContext(const SystemContextRequest& request) const
    {
        VE_ASSERT(m_ContextFactory != nullptr, "WorldRunner::BuildContext: no context factory");
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

        // End-play before teardown: run each system's OnStop while its scene is still live, symmetric
        // with a simulation start. Destructing the world (Scene::~Scene → the systems) runs no
        // OnStop — a destructor has no SystemContext to supply — so a system that releases an
        // engine-owned resource in OnStop depends on this stop, not on destruction. A runner with no
        // factory drops a started world without running OnStop rather than fabricating a context.
        // Stopping is idempotent, so a caller that already stopped takes a harmless no-op here.
        const World& closing = **it;
        Scene& scene = closing.GetScene();
        if (const SceneSimulation* sim = scene.GetSimulation();
            sim != nullptr && sim->IsStarted() && m_ContextFactory)
        {
            VE_PROFILE_SCOPE("World/Stop");
            scene.StopSimulation(BuildContext(SystemContextRequest{
                .World = closing.Id, .Scene = scene, .Phase = SystemContextPhase::Stop}));
        }

        if (m_SceneRetiringHook)
        {
            m_SceneRetiringHook(scene);
        }

        // Erase by id rather than through the iterator found above: a system's OnStop may close a
        // world itself, and an immediate close of another world moves this one's slot out from under
        // a held iterator (one issued inside a tick is queued instead, and cannot).
        VE_PROFILE_SCOPE("World/Destroy");
        std::erase_if(m_Worlds, [world](const Unique<World>& w) { return w->Id == world; });
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
        if (resolved->OwnedScene != nullptr && m_SceneRetiringHook)
        {
            m_SceneRetiringHook(*resolved->OwnedScene);
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
            const bool active =
                sim != nullptr && sim->IsStarted() && !sim->IsPaused() && !world->IsPaused();
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

    void WorldRunner::SetWorldPaused(const WorldInstanceId world, const bool paused)
    {
        if (World* resolved = ResolveWorld(world); resolved != nullptr)
        {
            resolved->ExplicitPaused = paused;
        }
    }

    bool WorldRunner::IsWorldPaused(const WorldInstanceId world) const
    {
        const World* resolved = ResolveWorld(world);
        return resolved != nullptr && resolved->IsPaused();
    }

    void WorldRunner::AcquirePause(const WorldInstanceId world)
    {
        if (World* resolved = ResolveWorld(world); resolved != nullptr)
        {
            ++resolved->PauseRefs;
        }
    }

    void WorldRunner::ReleasePause(const WorldInstanceId world)
    {
        if (World* resolved = ResolveWorld(world); resolved != nullptr && resolved->PauseRefs > 0)
        {
            --resolved->PauseRefs;
        }
    }

    WorldPauseScope WorldRunner::PauseScope(const WorldInstanceId world)
    {
        if (ResolveWorld(world) == nullptr)
        {
            return WorldPauseScope{};
        }
        AcquirePause(world);
        return WorldPauseScope(*this, world);
    }

    WorldCaptureDriveResult WorldRunner::DriveCaptureSurfaces(const WorldCaptureDriveInfo& info)
    {
        VE_ASSERT(
            info.Register != nullptr && info.IsPresented != nullptr,
            "WorldRunner::DriveCaptureSurfaces needs both a Register and an IsPresented hook");

        WorldCaptureDriveResult result;
        u32 built = 0;

        // Pause is not what gates capture driving: a paused world a viewport still presents drives its
        // mirrors. Presentation is — a capture feeds a material sampled by a mesh drawn in some view,
        // so a world no view shows has nowhere its capture could be seen.
        for (const Unique<World>& world : m_Worlds)
        {
            if (!info.IsPresented(world->Id))
            {
                ++result.WorldsSkipped;
                result.SurfacesReArmed += ReArmCaptureSurfaces(*world);
                continue;
            }
            ++result.WorldsDriven;

            Scene& scene = world->GetScene();
            // The world's own interpolation fraction, not the frame's: the drive walks every world,
            // and each advances its Sim on its own clock.
            const f32 alpha = world->LastAlpha;

            for (auto [entity, surface] : scene.View<Renderer::CaptureSurface>())
            {
                // A disabled surface holds nothing: releasing its runtime returns the capture to the
                // pool and clears the slots it bound, as removing the component would.
                if (!surface.Enabled)
                {
                    surface.Release();
                    ++result.SurfacesDisabled;
                    continue;
                }

                // A capture installed this pass — built or taken from the pool — is registered
                // below; one the surface already held is on the drive-list.
                const bool fresh = surface.GetCapture() == nullptr;
                if (fresh)
                {
                    VE_ASSERT(m_Context != nullptr && m_Assets != nullptr,
                              "WorldRunner::DriveCaptureSurfaces: driving a presented world's "
                              "capture surface needs a context and asset manager");
                    if (!MaterializeCapture(surface, info.MaxNewCaptures, built, result))
                    {
                        ++result.SurfacesDeferred;
                        continue;
                    }
                }

                // The capture renders from the pose the entity is *drawn* at (a probe centered on it,
                // a mirror placed at it) — the same pose the mesh it feeds is drawn at, since the
                // renderer blends a drawn transform between the last two Sim ticks by this alpha.
                // Resolving the un-interpolated pose instead puts the probe a partial tick from that
                // mesh and from everything else rigidly attached to it, by an offset that reopens and
                // collapses each tick as the alpha sweeps and grows with speed and turn rate.
                const mat4 drawTransform = scene.GetInterpolatedWorldTransform(entity, alpha);
                const vec3 position = vec3(drawTransform[3]);

                // An Entity-aligned capture orients its faces in the carrier's own frame, so a
                // body-fixed environment stays still in the map as the body turns; a World-aligned one
                // keeps the identity and renders along fixed world axes. The basis is the draw
                // transform's rotation with any scale divided out.
                mat3 faceBasis(1.0f);
                if (surface.Alignment == Renderer::CaptureAlignment::Entity)
                {
                    faceBasis = mat3(drawTransform);
                    faceBasis[0] = glm::normalize(faceBasis[0]);
                    faceBasis[1] = glm::normalize(faceBasis[1]);
                    faceBasis[2] = glm::normalize(faceBasis[2]);
                }

                // The surface's material is the sibling MeshRenderer's first — but the capture
                // writes its probe output (a texture handle, and the ProbeCenter validity flag the
                // shader gates the reflection on) into it, so binding onto the shared mesh-asset
                // instance would make every other entity drawing that mesh sample this probe. Draw
                // this entity through a private clone instead: on first drive, clone materials[0]
                // and install a per-entity InstanceMaterials override the render gather honours, so
                // the write reaches only this entity while every sharer keeps the untouched asset
                // instance. The one mutable access bumps the scene's spatial version once, which the
                // broadphase re-gathers on — subsequent frames read the installed clone const.
                AssetHandle<MaterialInstance> material;
                if (const auto* renderer = std::as_const(scene).TryGet<MeshRenderer>(entity);
                    renderer != nullptr && renderer->Mesh.IsLoaded())
                {
                    if (!renderer->InstanceMaterials.empty())
                    {
                        material = renderer->InstanceMaterials[0];
                    }
                    else if (const std::span<const AssetHandle<MaterialInstance>> meshMaterials =
                                 renderer->Mesh.Get()->GetMaterials();
                             !meshMaterials.empty() && meshMaterials[0].IsLoaded())
                    {
                        vector<AssetHandle<MaterialInstance>> overrides(meshMaterials.begin(),
                                                                        meshMaterials.end());
                        overrides[0] =
                            m_Assets->Adopt<MaterialInstance>(meshMaterials[0].Get()->Clone(
                                meshMaterials[0].Get()->GetName() + " (capture)"));
                        material = overrides[0];
                        scene.Get<MeshRenderer>(entity).InstanceMaterials = std::move(overrides);
                    }
                }

                // Register the capture on first materialization; the SceneCapture erases its own
                // pointer on destruction, so removing the component/entity/scene unregisters it.
                VE_ASSERT(m_Context != nullptr && m_Assets != nullptr,
                          "WorldRunner::DriveCaptureSurfaces: driving a presented world's capture "
                          "surface needs a context and asset manager");
                Renderer::SceneCapture* capture = surface.Drive(
                    *m_Context, *m_Assets, scene, entity, position, alpha, faceBasis, material);
                ++result.SurfacesDriven;
                if (capture != nullptr && fresh)
                {
                    info.Register(*capture);
                }
            }
        }

        return result;
    }

    bool WorldRunner::MaterializeCapture(const Renderer::CaptureSurface& surface, const u32 maxNew,
                                         u32& built, WorldCaptureDriveResult& result)
    {
        const Renderer::SceneCaptureInfo captureInfo =
            surface.GetCaptureInfo(*m_Context, *m_Assets);
        Unique<Renderer::SceneCapture> capture = m_CapturePool->Take(captureInfo);
        if (capture != nullptr)
        {
            ++result.CapturesReused;
        }
        else
        {
            if (built >= maxNew)
            {
                return false;
            }
            VE_PROFILE_SCOPE("Capture/Materialize");
            capture = Renderer::SceneCapture::Create(captureInfo);
            ++built;
            ++result.CapturesBuilt;
        }
        surface.Materialize(*m_Context, std::move(capture), m_CapturePool);
        return true;
    }

    u32 WorldRunner::ReArmCaptureSurfaces(const World& world)
    {
        // Only a capture that has already rendered holds content to go stale; one that never
        // materialized has nothing to re-arm and materializing it here would allocate for a world
        // nothing is looking at. An EveryFrame capture refreshes on its own, so the re-arm is what
        // an OnDemand one needs to not resume mid-refresh from a scene that has since moved on.
        u32 reArmed = 0;
        for (auto [entity, surface] : world.GetScene().View<Renderer::CaptureSurface>())
        {
            if (surface.GetCapture() != nullptr)
            {
                surface.MarkDirty();
                ++reArmed;
            }
        }
        return reArmed;
    }
}
