#include <Veng/Asset/Level.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneSimulation.h>

namespace Veng
{
    Ref<Level> Level::Create(AssetHandle<Prefab> world, vector<SystemId> systems,
                             GameModeConfig gameMode, LevelRenderSettings render)
    {
        return Ref<Level>(new Level(std::move(world), std::move(systems), std::move(gameMode),
                                    std::move(render)));
    }

    Level::Level(AssetHandle<Prefab> world, vector<SystemId> systems, GameModeConfig gameMode,
                 LevelRenderSettings render)
        : m_World(std::move(world)), m_Systems(std::move(systems)), m_GameMode(std::move(gameMode)),
          m_Render(std::move(render))
    {
    }

    LevelInstance Level::LoadInto(AssetManager& manager, const SystemRegistry& registry) const
    {
        VE_ASSERT(m_World.IsLoaded(), "Level::LoadInto: world prefab {} is not resident",
                  m_World.Id().Value);

        LevelInstance instance{
            .World = Scene::Create(manager.GetTypeRegistry()),
            .Simulation = CreateUnique<SceneSimulation>(registry, m_Systems),
        };

        // The spawned root entities are not retained — the world prefab authors its own
        // hierarchy and the simulation queries the scene, not the root list.
        (void)m_World.Get()->SpawnInto(*instance.World, manager);

        // The Session entity is level-scoped, not world-content, so the loader seeds it from
        // the level's game-mode config rather than the world authoring it.
        SeedSession(*instance.World, m_GameMode);

        return instance;
    }

    void SeedSession(Scene& scene, const GameModeConfig& gameMode)
    {
        const Entity session = scene.CreateEntity();
        scene.Add<Session>(session, Session{.Phase = SessionPhase::Playing});
        scene.Add<GameModeConfig>(session, gameMode);
    }
}
