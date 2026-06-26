#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Asset/ResidencyBatch.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/SceneSystem.h>

namespace Veng
{
    class Scene;
    class AssetManager;
    class TypeRegistry;
    class SystemRegistry;
    class SceneSimulation;
    class Prefab;

    /// @brief A running game: the Scene a level spawned, with its driving SceneSimulation attached.
    ///
    /// Returned by Level::LoadInto — the bundle an app owns and drives. The Scene holds the
    /// spawned world plus the seeded Session entity, and owns the SceneSimulation built from the
    /// level's ordered system set (Scene::GetSimulation / TickSimulation). Pending is the world
    /// spawn's residency batch (its recipe-built meshes streaming in). The app ticks the scene's
    /// simulation and renders the scene, optionally waiting on Pending before a deterministic
    /// capture; it drops the Scene in OnDispose like any other engine resource. The Scene outlives
    /// nothing it was built from except the TypeRegistry, which must outlive it.
    struct LevelInstance
    {
        /// @brief The runtime ECS world the level spawned its world prefab into, with its
        ///        SceneSimulation attached (Scene::GetSimulation).
        Unique<Scene> World;
        /// @brief The world spawn's not-yet-resident assets; wait on it before a deterministic capture.
        ResidencyBatch Pending;
    };

    /// @brief Cached, immutable cooked-level asset: a world prefab by reference plus level-scoped wiring.
    ///
    /// A Level does not embed world entities — it references a world Prefab and adds the data
    /// that is not reusable-recipe data: the ordered active system set, the game-mode config,
    /// and a render-settings subset. Loading a level (LoadInto) spawns its world into a fresh
    /// Scene, builds a SceneSimulation from the system set, and seeds a Session entity from the
    /// game-mode config — that is starting the game.
    ///
    /// Level is CPU data with no GPU resource. Load it through AssetManager::Load like any
    /// other asset; its world prefab and the prefab's embedded asset refs resolve as ordinary
    /// load-time dependencies.
    class Level
    {
    public:
        /// @brief Creates a Level from its resolved world prefab and decoded level-scoped config.
        ///
        /// @param world     The resolved world-prefab handle (kept resident for the level's lifetime).
        /// @param systems   The ordered active SystemId set, in run order.
        /// @param gameMode  The decoded game-mode config seeded onto the Session entity at load.
        /// @param render    The decoded render-settings subset the app maps onto the renderer.
        /// @return The constructed Level.
        static Ref<Level> Create(AssetHandle<Prefab> world, vector<SystemId> systems,
                                 GameModeConfig gameMode, LevelRenderSettings render);

        /// @brief Spawns the world, builds the simulation, and seeds the Session — starting the game.
        ///
        /// Creates a fresh Scene, spawns the world prefab into it (Prefab::SpawnInto), constructs
        /// a SceneSimulation from the level's ordered SystemId set against `registry` (honoring the
        /// Sim/View phases) and attaches it to the Scene (Scene::SetSimulation), and creates one
        /// Session entity carrying a Playing Session plus the level's game-mode config. Returns the
        /// bundle the app drives. The simulation is returned not-yet-started — the caller calls
        /// Scene::StartSimulation.
        /// @param manager   The asset manager the world spawns and its dependencies resolve through.
        /// @param registry  The host-owned system catalog the level's ids resolve against.
        /// @pre The world prefab handle IsLoaded().
        /// @return The Scene (with its SceneSimulation attached) + residency bundle.
        [[nodiscard]] LevelInstance LoadInto(AssetManager& manager,
                                             const SystemRegistry& registry) const;

        /// @brief Returns the world-prefab handle this level references.
        [[nodiscard]] const AssetHandle<Prefab>& GetWorld() const { return m_World; }

        /// @brief Returns the ordered active SystemId set, in run order.
        [[nodiscard]] const vector<SystemId>& GetSystems() const { return m_Systems; }

        /// @brief Returns the game-mode config seeded onto the Session entity at load.
        [[nodiscard]] const GameModeConfig& GetGameMode() const { return m_GameMode; }

        /// @brief Returns the render-settings subset the app maps onto the renderer.
        [[nodiscard]] const LevelRenderSettings& GetRender() const { return m_Render; }

    private:
        Level(AssetHandle<Prefab> world, vector<SystemId> systems, GameModeConfig gameMode,
              LevelRenderSettings render);

        /// @brief The referenced world prefab, kept resident for the level's lifetime.
        AssetHandle<Prefab> m_World;
        /// @brief The ordered active SystemId set, in run order.
        vector<SystemId> m_Systems;
        /// @brief The game-mode config seeded onto the Session entity at load.
        GameModeConfig m_GameMode;
        /// @brief The render-settings subset the app maps onto the renderer.
        LevelRenderSettings m_Render;
    };

    /// @brief Materializes a level's runtime config onto a settings entity in the scene.
    ///
    /// Adds one entity carrying a Playing Session plus the level's @p gameMode and @p render —
    /// the level's authored config made available as scene components, so rule systems read the
    /// game mode (and Session) and the engine reads the render settings by querying the scene
    /// (Scene::TryGetFirst), never assuming a particular entity. The configs stay authored on the
    /// Level (and edited as separate level-editor panels); this is where that data enters the
    /// running world. Level::LoadInto calls this after spawning the world; the editor's Play
    /// calls it too, so a play session reaches the same initialized state the runtime does.
    /// @param scene     The scene the settings entity is created in.
    /// @param gameMode  The game-mode config carried beside the Playing Session.
    /// @param render    The render settings the engine resolves onto the renderer.
    void SeedLevel(Scene& scene, const GameModeConfig& gameMode, const LevelRenderSettings& render);

    /// @brief AssetTypeTrait specialization mapping Level to AssetType::Level.
    template <>
    struct AssetTypeTrait<Level>
    {
        /// @brief The asset type tag for Level.
        static constexpr AssetType Type = AssetType::Level;
    };
}
