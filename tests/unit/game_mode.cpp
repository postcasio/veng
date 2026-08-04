// Game mode as data: a GameModeConfig settings component plus a Sim-phase spawn rule, driven
// by the real SceneSimulation. Pure CPU — no Context, no Vulkan symbol touched; builds a real
// Scene over a TypeRegistry. The spawn rule here mirrors the example's SpawnPlayerRule policy
// (spawn the player wiring when the scene carries a config, despawn on Stop) so the suite
// asserts the lifecycle placement without linking the game module or a device. A game with
// richer mode state authors its own components beside the config and rule systems that read
// them.
//
// The example's rule spawns from a cooked player prefab; spawning a prefab needs an
// AssetManager (hence a Context). This test spawns the equivalent wiring directly so the
// rule's config gate and despawn lifecycle are exercised device-free — the prefab-spawn
// path itself is covered in the gpu band.

#include <doctest/doctest.h>

#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/CameraRig.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneSimulation.h>
#include <Veng/Scene/SceneSystem.h>
#include <Veng/Scene/SystemRegistry.h>

using namespace Veng;

namespace
{
    TypeRegistry MakeRegistry()
    {
        TypeRegistry registry;
        RegisterBuiltinTypes(registry);
        return registry;
    }

    // A SystemContext the driver forwards but no system here dereferences (the spawn mirrors
    // the player prefab inline rather than loading one, so it never touches the AssetManager).
    struct ContextStorage
    {
        alignas(16) unsigned char AssetsBytes[64]{};
        alignas(16) unsigned char InputBytes[64]{};
        alignas(16) unsigned char TasksBytes[64]{};

        SystemContext Make()
        {
            return SystemContext{
                .Assets = *reinterpret_cast<AssetManager*>(AssetsBytes),
                .Input = *reinterpret_cast<Input*>(InputBytes),
                .Tasks = *reinterpret_cast<TaskSystem*>(TasksBytes),
                .Audio = *reinterpret_cast<Audio::AudioEngine*>(TasksBytes),
            };
        }
    };

    // The spawn rule under test, mirroring the example's SpawnPlayerRule: a Sim-phase system
    // that, when the scene carries a GameModeConfig, spawns the player wiring (a camera, a seat
    // that possesses the pawn and views through the camera, and the pawn) at start and tears it
    // down on OnStop. It picks nothing here — the wiring is fixed — but exercises the same
    // config gate and despawn lifecycle.
    class TestSpawnPlayerRule final : public SceneSystem
    {
    public:
        void OnStart(Scene& scene, const SystemContext&) override
        {
            if (FindConfig(scene) == Entity::Null)
            {
                return;
            }

            const Entity pawn = scene.CreateEntity();
            scene.Add<Name>(pawn, Name{.Value = "Player Pawn"});
            scene.Add<Transform>(pawn, Transform{.Position = vec3(0.0f, 1.0f, 0.0f)});
            scene.Add<Intent>(pawn, Intent{});
            scene.Add<Mover>(pawn, Mover{});
            scene.Add<Authority>(pawn, Authority{.Tier = Tier::Server});

            const Entity camera = scene.CreateEntity();
            scene.Add<Name>(camera, Name{.Value = "Player Camera"});
            scene.Add<Transform>(camera, Transform{.Position = vec3(0.0f, 10.0f, 14.0f)});
            scene.Add<Camera>(camera, Camera{});
            scene.Add<CameraFollow>(
                camera, CameraFollow{.Target = pawn, .Offset = vec3(0.0f, 6.0f, 12.0f)});
            scene.Add<Authority>(camera, Authority{.Tier = Tier::Local});
            m_Camera = camera;

            const Entity seat = scene.CreateEntity();
            scene.Add<Name>(seat, Name{.Value = "Player Seat"});
            scene.Add<Viewer>(seat, Viewer{.Camera = camera});
            scene.Add<PlayerInput>(seat, PlayerInput{});
            scene.Add<Possesses>(seat, Possesses{.Pawn = pawn});
            scene.Add<Authority>(seat, Authority{.Tier = Tier::Local});

            m_Spawned = {camera, pawn, seat};
        }

        void OnStop(Scene& scene, const SystemContext&) override { Despawn(scene); }

        // The spawn happens once at OnStart and the teardown at OnStop; no per-tick rule action.
        void OnUpdate(Scene&, const f32, const SystemContext&) override {}

        [[nodiscard]] Entity GetSpawnedCamera() const { return m_Camera; }

    private:
        static Entity FindConfig(Scene& scene)
        {
            Entity found = Entity::Null;
            scene.Each<GameModeConfig>([&found](const Entity entity, GameModeConfig&)
                                       { found = entity; });
            return found;
        }

        void Despawn(Scene& scene)
        {
            for (const Entity entity : m_Spawned)
            {
                if (scene.IsAlive(entity))
                {
                    scene.DestroyEntity(entity);
                }
            }
            m_Spawned.clear();
            m_Camera = Entity::Null;
        }

        Entity m_Camera = Entity::Null;
        vector<Entity> m_Spawned;
    };

    // Adds the settings entity carrying the game-mode config the rule gates on.
    Entity AddConfig(Scene& scene)
    {
        const Entity settings = scene.CreateEntity();
        scene.Add<GameModeConfig>(settings, GameModeConfig{});
        scene.Add<Authority>(settings, Authority{.Tier = Tier::Server});
        return settings;
    }

    // Counts entities possessing a live pawn.
    int CountPossessedSeats(Scene& scene)
    {
        int count = 0;
        scene.Each<Possesses>(
            [&](Entity, Possesses& possesses)
            {
                if (possesses.Pawn != Entity::Null && scene.IsAlive(possesses.Pawn))
                {
                    ++count;
                }
            });
        return count;
    }
}

VE_SYSTEM(TestSpawnPlayerRule, 0xEF01A0C1C1F79775ULL, "Test Spawn Player Rule");

TEST_CASE("A spawn rule instantiates the possessed player when the scene carries a config")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);
    AddConfig(*scene);

    SystemRegistry systems;
    systems.Register<TestSpawnPlayerRule>();
    SceneSimulation sim(systems);

    ContextStorage storage;
    sim.Start(*scene, storage.Make());

    // After Start the scene holds a possessed pawn and a Viewer naming a (Transform, Camera).
    CHECK(CountPossessedSeats(*scene) == 1);

    Entity viewerCamera = Entity::Null;
    scene->Each<Viewer>([&](Entity, Viewer& viewer) { viewerCamera = viewer.Camera; });
    REQUIRE(viewerCamera != Entity::Null);
    REQUIRE(scene->IsAlive(viewerCamera));
    CHECK(scene->Has<Camera>(viewerCamera));
    CHECK(scene->Has<Transform>(viewerCamera));
}

TEST_CASE("Stopping the simulation despawns the spawned player")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);
    AddConfig(*scene);

    SystemRegistry systems;
    systems.Register<TestSpawnPlayerRule>();
    SceneSimulation sim(systems);

    ContextStorage storage;
    sim.Start(*scene, storage.Make());
    REQUIRE(CountPossessedSeats(*scene) == 1);

    sim.Stop(*scene, storage.Make());

    // The player wiring is gone: no seat, pawn, or camera survives.
    int seats = 0;
    scene->Each<Possesses>([&](Entity, Possesses&) { ++seats; });
    CHECK(seats == 0);
    int cameras = 0;
    scene->Each<Camera>([&](Entity, Camera&) { ++cameras; });
    CHECK(cameras == 0);
}

TEST_CASE("A scene with no game-mode config runs the rule unchanged — nothing spawns")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);

    // A bare static entity, no GameModeConfig.
    const Entity prop = scene->CreateEntity();
    scene->Add<Transform>(prop, Transform{});

    SystemRegistry systems;
    systems.Register<TestSpawnPlayerRule>();
    SceneSimulation sim(systems);

    ContextStorage storage;
    sim.Start(*scene, storage.Make());
    sim.Update(*scene, 0.016f, storage.Make());

    // No player wiring appeared.
    int seats = 0;
    scene->Each<Possesses>([&](Entity, Possesses&) { ++seats; });
    CHECK(seats == 0);
    CHECK(scene->IsAlive(prop));
}

TEST_CASE("The spawn rule runs in the Sim phase, and its camera is trailed by the View-phase rig")
{
    TypeRegistry registry = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(registry);
    AddConfig(*scene);

    // The spawn rule is Sim-phase (the default), the camera rig is View-phase, so the rig
    // trails the spawned camera in the same tick the Sim phase finalized the pawn.
    auto rule = CreateUnique<TestSpawnPlayerRule>();
    CHECK(rule->GetPhase() == SceneSystem::Phase::Sim);
    CHECK(CameraRigSystem{}.GetPhase() == SceneSystem::Phase::View);

    SystemRegistry systems;
    systems.Register<TestSpawnPlayerRule>();
    systems.Register<CameraRigSystem>();
    SceneSimulation sim(systems);

    ContextStorage storage;
    sim.Start(*scene, storage.Make());

    // Find the spawned camera + pawn through the seat's references.
    Entity camera = Entity::Null;
    Entity pawn = Entity::Null;
    scene->Each<Viewer, Possesses>(
        [&](Entity, Viewer& viewer, Possesses& possesses)
        {
            camera = viewer.Camera;
            pawn = possesses.Pawn;
        });
    REQUIRE(camera != Entity::Null);
    REQUIRE(pawn != Entity::Null);

    // The pawn sits away from the camera's authored pose; one Update runs the View-phase
    // rig, which moves the camera to trail the pawn (snap: the CameraFollow Damping is 0).
    const vec3 cameraBefore = scene->Get<Transform>(camera).Position;
    sim.Update(*scene, 0.016f, storage.Make());
    const vec3 cameraAfter = scene->Get<Transform>(camera).Position;

    // The rig wrote the camera transform — it no longer sits at its authored pose.
    CHECK(glm::any(glm::greaterThan(glm::abs(cameraAfter - cameraBefore), vec3(1e-4f))));
}
