#include <Veng/WorldRunner.h>

#include <Veng/Assert.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Renderer/CaptureSurface.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneSimulation.h>
#include <Veng/Scene/Transforms.h>

#include <algorithm>
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

    WorldRunner::~WorldRunner()
    {
        // Adopted scenes are externally owned and may outlive the runner; clear each one's
        // back-reference to this runner's drive-list so its own ~Scene does not erase from the freed
        // vector. Owned worlds' scenes were never attached, so they need no detach.
        for (Scene* scene : m_AdoptedScenes)
        {
            scene->DetachFromSimDriveList();
        }
    }

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
            if (info.EmptySimulation)
            {
                world->OwnedScene->SetSimulation(CreateUnique<SceneSimulation>(*m_Systems));
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

    WorldInstanceId WorldRunner::AdoptSimulation(Scene& scene)
    {
        auto world = CreateUnique<World>();
        world->Id = MintId();
        world->LiveScene = &scene;
        world->Adopted = true;
        world->Clock = SimClock(SimClockInfo{});
        const WorldInstanceId id = world->Id;
        m_Worlds.push_back(std::move(world));

        m_AdoptedScenes.push_back(&scene);
        scene.AttachToSimDriveList(m_AdoptedScenes);
        return id;
    }

    Scene& WorldRunner::InstallScene(const WorldInstanceId world, Unique<Scene> scene)
    {
        World* resolved = ResolveWorld(world);
        VE_ASSERT(resolved != nullptr, "WorldRunner::InstallScene: unminted world");
        resolved->OwnedScene = std::move(scene);
        resolved->LiveScene = resolved->OwnedScene.get();
        resolved->Adopted = false;
        return *resolved->LiveScene;
    }

    void WorldRunner::PruneAdopted()
    {
        // An adopted world's scene self-erases from m_AdoptedScenes when it is destroyed, so an
        // adopted world whose LiveScene no longer appears there names a dead scene — drop it. The
        // pointer is compared, never dereferenced, so a dangling LiveScene is safe here.
        std::erase_if(m_Worlds,
                      [this](const Unique<World>& w)
                      {
                          return w->Adopted && std::ranges::find(m_AdoptedScenes, w->LiveScene) ==
                                                   m_AdoptedScenes.end();
                      });
    }

    WorldTickResult WorldRunner::Tick(const WorldTickInfo& info)
    {
        PruneAdopted();

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

            if (info.RunViewPhase)
            {
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

    void WorldRunner::DriveCaptureSurfaces(
        const function<void(Renderer::SceneCapture&)>& registerCapture)
    {
        VE_ASSERT(m_Context != nullptr && m_Assets != nullptr,
                  "WorldRunner::DriveCaptureSurfaces needs a context and asset manager");

        // An adopted world whose scene was destroyed since the last Tick still lingers in m_Worlds
        // with a dangling LiveScene until pruned; drop those before dereferencing any scene here (this
        // runs after the frame's OnUpdate, which may have dropped an overlay's scene).
        PruneAdopted();

        // Registration gates capture driving, not run-state: iterate every world regardless of
        // started/paused, so a paused world still drives its mirrors and a view-less world's captures
        // are engine-driven.
        for (const Unique<World>& world : m_Worlds)
        {
            const Scene& scene = world->GetScene();
            for (auto [entity, surface] : scene.View<Renderer::CaptureSurface>())
            {
                // The capture renders from the entity's world position (a probe centered on it, a
                // mirror placed at it). The surface's material is the sibling MeshRenderer's first.
                const vec3 position = vec3(WorldMatrix(scene, entity)[3]);

                MaterialInstance* material = nullptr;
                if (const MeshRenderer* mesh = scene.TryGet<MeshRenderer>(entity); mesh != nullptr)
                {
                    if (mesh->Mesh.IsLoaded())
                    {
                        const std::span<const AssetHandle<MaterialInstance>> materials =
                            mesh->Mesh.Get()->GetMaterials();
                        if (!materials.empty() && materials[0].IsLoaded())
                        {
                            material = materials[0].Get();
                        }
                    }
                }

                // Register the capture on first materialization; the SceneCapture erases its own
                // pointer on destruction, so removing the component/entity/scene unregisters it.
                const bool hadCapture = surface.GetCapture() != nullptr;
                Renderer::SceneCapture* capture =
                    surface.Drive(*m_Context, *m_Assets, scene, position, material);
                if (capture != nullptr && !hadCapture)
                {
                    registerCapture(*capture);
                }
            }
        }
    }
}
