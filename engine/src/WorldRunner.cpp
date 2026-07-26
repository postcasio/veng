#include <Veng/WorldRunner.h>

#include <Veng/Assert.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Diagnostics/Profiler.h>
#include <Veng/Log.h>
#include <Veng/Renderer/CaptureSurface.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneSimulation.h>

#include <algorithm>
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
    }

    WorldRunner::~WorldRunner() = default;

    WorldInstanceId WorldRunner::MintId()
    {
        return WorldInstanceId{.Value = m_NextId++};
    }

    WorldInstanceId WorldRunner::OpenWorld(const WorldOpenInfo& info)
    {
        auto world = CreateUnique<World>();
        world->Id = MintId();
        world->Clock = SimClock(SimClockInfo{.TickRate = info.SimTickRate});

        if (info.Source.IsLoaded())
        {
            VE_ASSERT(m_Assets != nullptr, "WorldRunner: opening a cooked-level world needs an "
                                           "AssetManager");
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
            info.OnLoaded(id, scene, pending);
        }

        if (info.StartSimulation && scene.GetSimulation() != nullptr)
        {
            VE_ASSERT(info.MakeStartContext != nullptr,
                      "WorldRunner: StartSimulation needs a MakeStartContext");
            scene.StartSimulation(info.MakeStartContext());
        }

        return id;
    }

    void WorldRunner::CloseWorld(const WorldInstanceId world)
    {
        const auto it = std::ranges::find_if(m_Worlds, [world](const Unique<World>& w)
                                             { return w->Id == world; });
        if (it == m_Worlds.end())
        {
            return;
        }
        m_Worlds.erase(it);
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
        resolved->OwnedScene = std::move(scene);
        resolved->LiveScene = resolved->OwnedScene.get();
        return *resolved->LiveScene;
    }

    WorldTickResult WorldRunner::Tick(const WorldTickInfo& info)
    {
        VE_PROFILE_SCOPE("WorldRunner/Tick");

        WorldTickResult result;
        for (const Unique<World>& world : m_Worlds)
        {
            Scene& scene = world->GetScene();
            const SceneSimulation* sim = scene.GetSimulation();
            const bool active =
                sim != nullptr && sim->IsStarted() && !sim->IsPaused() && !world->IsPaused();
            if (!active)
            {
                // A paused or unstarted world drops its accumulator so resuming chases no backlog.
                world->Clock.Reset();
                world->LastAlpha = 0.0f;
                continue;
            }
            result.AnyActive = true;

            const f32 scale = info.SimScale ? info.SimScale(world->Id) : 1.0f;
            const SimStep step = world->Clock.Advance(info.Delta * scale);
            world->LastAlpha = step.Alpha;
            if (step.Steps > 0)
            {
                result.AnyTicked = true;
            }

            {
                // Per-world Sim scope, named with the world's identity so several worlds read side
                // by side rather than summed. The step counter distinguishes a heavy simulation from
                // a frame that spiralled into multiple fixed-step catch-up steps.
                const string simLabel = "World " + std::to_string(world->Id.Value) + " Sim";
                VE_PROFILE_SCOPE_DYNAMIC(simLabel);
                VE_PROFILE_COUNTER("WorldRunner/SimSteps", static_cast<f64>(step.Steps));

                for (u32 tickIndex = 0; tickIndex < step.Steps; ++tickIndex)
                {
                    const u64 tick = step.FirstTick + tickIndex;
                    if (info.BeforeSimStep)
                    {
                        info.BeforeSimStep(world->Id, scene, tick);
                    }
                    scene.TickSimulationPhase(
                        SceneSystem::Phase::Sim, step.SimDelta,
                        info.BuildContext(world->Id, scene, tick, 0.0f, tickIndex == 0));
                    if (info.AfterSimStep)
                    {
                        info.AfterSimStep(world->Id, scene, tick);
                    }
                }
            }

            if (info.RunViewPhase)
            {
                const string viewLabel = "World " + std::to_string(world->Id.Value) + " View";
                VE_PROFILE_SCOPE_DYNAMIC(viewLabel);
                scene.TickSimulationPhase(
                    SceneSystem::Phase::View, info.Delta,
                    info.BuildContext(world->Id, scene, world->Clock.GetTick(), step.Alpha, false));
            }
        }
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

        // The MaterialInstances driven this pass, so a second capture binding onto one already bound is
        // reported rather than silently winning: the target is the sibling mesh *asset*'s cooked
        // instance, shared by every entity drawing that asset, so two such entities have one slot
        // between them.
        vector<const MaterialInstance*> boundMaterials;

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

            const Scene& scene = world->GetScene();
            // The world's own interpolation fraction, not the frame's: the drive walks every world,
            // and each advances its Sim on its own clock.
            const f32 alpha = world->LastAlpha;

            for (auto [entity, surface] : scene.View<Renderer::CaptureSurface>())
            {
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

                // The surface's material is the sibling MeshRenderer's first.
                AssetHandle<MaterialInstance> material;
                if (const auto* mesh = scene.TryGet<MeshRenderer>(entity); mesh != nullptr)
                {
                    if (mesh->Mesh.IsLoaded())
                    {
                        const std::span<const AssetHandle<MaterialInstance>> materials =
                            mesh->Mesh.Get()->GetMaterials();
                        if (!materials.empty() && materials[0].IsLoaded())
                        {
                            material = materials[0];
                        }
                    }
                }

                if (const MaterialInstance* const target = material.Get(); target != nullptr)
                {
                    if (std::ranges::find(boundMaterials, target) != boundMaterials.end())
                    {
                        if (!m_WarnedSharedCaptureMaterial)
                        {
                            m_WarnedSharedCaptureMaterial = true;
                            Log::Warn("Two CaptureSurfaces drive material '{}' in one frame: it "
                                      "belongs to a shared mesh asset, so they overwrite each "
                                      "other's output and both sample one probe. Give each "
                                      "capturing entity its own mesh asset or material instance.",
                                      target->GetName());
                        }
                    }
                    else
                    {
                        boundMaterials.emplace_back(target);
                    }
                }

                // Register the capture on first materialization; the SceneCapture erases its own
                // pointer on destruction, so removing the component/entity/scene unregisters it.
                VE_ASSERT(m_Context != nullptr && m_Assets != nullptr,
                          "WorldRunner::DriveCaptureSurfaces: driving a presented world's capture "
                          "surface needs a context and asset manager");
                const bool hadCapture = surface.GetCapture() != nullptr;
                Renderer::SceneCapture* capture = surface.Drive(
                    *m_Context, *m_Assets, scene, entity, position, alpha, faceBasis, material);
                ++result.SurfacesDriven;
                if (capture != nullptr && !hadCapture)
                {
                    info.Register(*capture);
                }
            }
        }

        return result;
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
