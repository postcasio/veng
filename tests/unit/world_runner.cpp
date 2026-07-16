// WorldRunner: the flat-peer world scheduler. Opens empty-scene worlds, ticks them independently by
// handle, resolves each by id, closes one without disturbing peers, and honors the pause refcount
// (an explicit toggle plus nested RAII PauseScopes). Pure CPU — device-free by construction: the
// runner is built with only a TypeRegistry + SystemRegistry (no AssetManager, no Context), so the
// empty-scene path spawns and drives worlds with no GPU.
//
// SystemContext aggregates device-bound services (AssetManager, Input, TaskSystem) none of which a
// unit test can construct; the probe system never dereferences the context, so a context over
// never-dereferenced storage keeps the whole test device-free while exercising the real Tick path.

#include <doctest/doctest.h>

#include <map>

#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneSimulation.h>
#include <Veng/Scene/SceneSystem.h>
#include <Veng/Scene/SystemRegistry.h>
#include <Veng/World.h>
#include <Veng/WorldRunner.h>

using namespace Veng;

namespace
{
    // A Sim-phase probe recording how many Sim steps each scene ran, keyed by scene pointer so two
    // worlds (each its own scene) are counted independently. Static, cleared per case with Reset.
    struct TickProbe final : SceneSystem
    {
        static inline std::map<const Scene*, int> Updates;

        static void Reset() { Updates.clear(); }

        void OnUpdate(Scene& scene, f32, const SystemContext&) override { ++Updates[&scene]; }
    };

    // A second registered probe the opener-named system set deliberately leaves out, proving an
    // empty world runs exactly the systems its opener names — never the whole registry.
    struct OtherProbe final : SceneSystem
    {
        static inline std::map<const Scene*, int> Updates;

        static void Reset() { Updates.clear(); }

        void OnUpdate(Scene& scene, f32, const SystemContext&) override { ++Updates[&scene]; }
    };
}

namespace Veng
{
    template <>
    struct VengSystem<TickProbe>
    {
        static constexpr SystemId Id = 0x0071D000000000A1ULL;
        static string Name() { return "TickProbe"; }
    };

    template <>
    struct VengSystem<OtherProbe>
    {
        static constexpr SystemId Id = 0x0071D000000000A2ULL;
        static string Name() { return "OtherProbe"; }
    };
}

namespace
{
    // A SystemContext the runner forwards but no system here dereferences; the storage is never read
    // as an AssetManager/Input/TaskSystem, it only backs the aggregate's references.
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
            };
        }
    };

    // A WorldOpenInfo for an empty-scene world running the opener-named probe, started with
    // the fake context — the device-free open the runner supports with no AssetManager.
    WorldOpenInfo EmptyWorld(ContextStorage& storage)
    {
        return WorldOpenInfo{
            .SimTickRate = 60,
            .StartSimulation = true,
            .Systems = vector<SystemId>{SystemIdOf<TickProbe>()},
            .MakeStartContext = [&storage] { return storage.Make(); },
        };
    }

    // A tick info that runs one Sim step per call (delta == the 60 Hz fixed step) and forwards the
    // fake context; the probe reads only the scene, so BuildContext's tick/alpha are immaterial here.
    WorldTickInfo OneStep(ContextStorage& storage)
    {
        return WorldTickInfo{
            .Delta = 1.0f / 60.0f,
            .BuildContext = [&storage](WorldInstanceId, const Scene&, u64, f32, bool)
            { return storage.Make(); },
        };
    }
}

TEST_CASE("A device-free WorldRunner opens two empty worlds and ticks both, resolved by id")
{
    TickProbe::Reset();

    TypeRegistry types;
    SystemRegistry systems;
    systems.Register<TickProbe>();

    // No AssetManager, no Context: the runner is device-free and drives only empty-scene worlds.
    WorldRunner runner(WorldRunnerInfo{.Types = &types, .Systems = &systems});

    ContextStorage storage;
    const WorldInstanceId a = runner.OpenWorld(EmptyWorld(storage));
    const WorldInstanceId b = runner.OpenWorld(EmptyWorld(storage));

    // Each open mints a distinct valid handle, and each resolves live to its own world (distinct
    // scenes) — the flat-peer addressing, never a privileged primary.
    CHECK(a.IsValid());
    CHECK(b.IsValid());
    CHECK_FALSE(a == b);
    const World* worldA = runner.ResolveWorld(a);
    const World* worldB = runner.ResolveWorld(b);
    REQUIRE(worldA != nullptr);
    REQUIRE(worldB != nullptr);
    CHECK(&worldA->GetScene() != &worldB->GetScene());

    for (int i = 0; i < 3; ++i)
    {
        runner.Tick(OneStep(storage));
    }

    // Both worlds advanced their own Sim by the same three steps, independently.
    CHECK(TickProbe::Updates[&worldA->GetScene()] == 3);
    CHECK(TickProbe::Updates[&worldB->GetScene()] == 3);
    CHECK(worldA->Clock.GetTick() == 3);
    CHECK(worldB->Clock.GetTick() == 3);
}

TEST_CASE("An empty world runs exactly its opener-named system set; an empty set runs none")
{
    TickProbe::Reset();
    OtherProbe::Reset();

    TypeRegistry types;
    SystemRegistry systems;
    systems.Register<TickProbe>();
    systems.Register<OtherProbe>();
    WorldRunner runner(WorldRunnerInfo{.Types = &types, .Systems = &systems});
    ContextStorage storage;

    // Named set: exactly TickProbe, though the registry also holds OtherProbe.
    const WorldInstanceId named = runner.OpenWorld(WorldOpenInfo{
        .SimTickRate = 60,
        .StartSimulation = true,
        .Systems = vector<SystemId>{SystemIdOf<TickProbe>()},
        .MakeStartContext = [&storage] { return storage.Make(); },
    });

    // Empty set: a simulation running no systems — the world still starts and its clock ticks
    // (the data-world shape: content arrives by other means, no system advances it).
    const WorldInstanceId bare = runner.OpenWorld(WorldOpenInfo{
        .SimTickRate = 60,
        .StartSimulation = true,
        .Systems = vector<SystemId>{},
        .MakeStartContext = [&storage] { return storage.Make(); },
    });

    // Disengaged: no simulation at all — the world holds a scene but never ticks.
    const WorldInstanceId simless = runner.OpenWorld(WorldOpenInfo{
        .SimTickRate = 60,
        .StartSimulation = true,
        .MakeStartContext = [&storage] { return storage.Make(); },
    });

    const Scene* namedScene = &runner.ResolveWorld(named)->GetScene();
    const Scene* bareScene = &runner.ResolveWorld(bare)->GetScene();
    CHECK(runner.ResolveWorld(simless)->GetScene().GetSimulation() == nullptr);

    for (int i = 0; i < 3; ++i)
    {
        runner.Tick(OneStep(storage));
    }

    // The named world ran exactly its named system: TickProbe stepped, OtherProbe never did.
    CHECK(TickProbe::Updates[namedScene] == 3);
    CHECK(OtherProbe::Updates.find(namedScene) == OtherProbe::Updates.end());

    // The empty-set world ticked its clock while running no systems at all.
    CHECK(TickProbe::Updates.find(bareScene) == TickProbe::Updates.end());
    CHECK(OtherProbe::Updates.find(bareScene) == OtherProbe::Updates.end());
    CHECK(runner.ResolveWorld(bare)->Clock.GetTick() == 3);

    // The simulation-less world never advanced.
    CHECK(runner.ResolveWorld(simless)->Clock.GetTick() == 0);
}

TEST_CASE("Closing a world resolves its id to nothing and leaves a peer untouched")
{
    TickProbe::Reset();

    TypeRegistry types;
    SystemRegistry systems;
    systems.Register<TickProbe>();
    WorldRunner runner(WorldRunnerInfo{.Types = &types, .Systems = &systems});

    ContextStorage storage;
    const WorldInstanceId a = runner.OpenWorld(EmptyWorld(storage));
    const WorldInstanceId b = runner.OpenWorld(EmptyWorld(storage));

    const Scene* sceneB = &runner.ResolveWorld(b)->GetScene();

    runner.CloseWorld(a);

    // The closed id resolves to nothing; the peer is untouched and still resolves.
    CHECK(runner.ResolveWorld(a) == nullptr);
    REQUIRE(runner.ResolveWorld(b) != nullptr);

    for (int i = 0; i < 2; ++i)
    {
        runner.Tick(OneStep(storage));
    }

    // Only the surviving world ticked (the closed world's scene was destroyed).
    CHECK(TickProbe::Updates[sceneB] == 2);
    CHECK(runner.ResolveWorld(b)->Clock.GetTick() == 2);
}

TEST_CASE("A paused world's sim does not advance while a peer's does")
{
    TickProbe::Reset();

    TypeRegistry types;
    SystemRegistry systems;
    systems.Register<TickProbe>();
    WorldRunner runner(WorldRunnerInfo{.Types = &types, .Systems = &systems});

    ContextStorage storage;
    const WorldInstanceId a = runner.OpenWorld(EmptyWorld(storage));
    const WorldInstanceId b = runner.OpenWorld(EmptyWorld(storage));

    const Scene* sceneA = &runner.ResolveWorld(a)->GetScene();
    const Scene* sceneB = &runner.ResolveWorld(b)->GetScene();

    runner.SetWorldPaused(a, true);
    CHECK(runner.IsWorldPaused(a));
    CHECK_FALSE(runner.IsWorldPaused(b));

    for (int i = 0; i < 3; ++i)
    {
        runner.Tick(OneStep(storage));
    }

    // The paused world ran no Sim step and its tick held; the peer advanced normally.
    CHECK(TickProbe::Updates.find(sceneA) == TickProbe::Updates.end());
    CHECK(runner.ResolveWorld(a)->Clock.GetTick() == 0);
    CHECK(TickProbe::Updates[sceneB] == 3);
    CHECK(runner.ResolveWorld(b)->Clock.GetTick() == 3);

    // Resuming ticks it again, and — because a paused world drops its accumulator — it chases no
    // backlog for the frames it sat paused.
    runner.SetWorldPaused(a, false);
    runner.Tick(OneStep(storage));
    CHECK(TickProbe::Updates[sceneA] == 1);
    CHECK(runner.ResolveWorld(a)->Clock.GetTick() == 1);
}

TEST_CASE("Nested PauseScopes hold a world paused until the last one drops")
{
    TickProbe::Reset();

    TypeRegistry types;
    SystemRegistry systems;
    systems.Register<TickProbe>();
    WorldRunner runner(WorldRunnerInfo{.Types = &types, .Systems = &systems});

    ContextStorage storage;
    const WorldInstanceId a = runner.OpenWorld(EmptyWorld(storage));
    const Scene* sceneA = &runner.ResolveWorld(a)->GetScene();

    CHECK_FALSE(runner.IsWorldPaused(a));
    {
        const WorldPauseScope outer = runner.PauseScope(a);
        CHECK(runner.IsWorldPaused(a));
        {
            const WorldPauseScope inner = runner.PauseScope(a);
            CHECK(runner.IsWorldPaused(a));
        }
        // The inner scope dropped, but the outer still holds the pause (refcount, not a boolean).
        CHECK(runner.IsWorldPaused(a));

        runner.Tick(OneStep(storage));
        CHECK(TickProbe::Updates.find(sceneA) == TickProbe::Updates.end());
    }
    // The last scope dropped: the world resumes.
    CHECK_FALSE(runner.IsWorldPaused(a));
    runner.Tick(OneStep(storage));
    CHECK(TickProbe::Updates[sceneA] == 1);
}

TEST_CASE("A held PauseScope composes with the explicit toggle without clobbering")
{
    TypeRegistry types;
    SystemRegistry systems;
    systems.Register<TickProbe>();
    WorldRunner runner(WorldRunnerInfo{.Types = &types, .Systems = &systems});

    ContextStorage storage;
    const WorldInstanceId a = runner.OpenWorld(EmptyWorld(storage));

    // A scope and the explicit toggle are separate reasons: clearing one while the other holds
    // leaves the world paused — the SeatFocusScope idiom, where a bare boolean would clobber.
    WorldPauseScope scope = runner.PauseScope(a);
    runner.SetWorldPaused(a, true);
    CHECK(runner.IsWorldPaused(a));

    runner.SetWorldPaused(a, false);
    CHECK(runner.IsWorldPaused(a)); // the scope still holds it

    scope = WorldPauseScope{}; // drop the scope by move-assigning an inert one
    CHECK_FALSE(runner.IsWorldPaused(a));
}
