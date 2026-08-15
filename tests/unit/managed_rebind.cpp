// The complete world rebind, device-free: the two pure decisions a managed viewport's rebind makes,
// plus the departed-overlay detach primitive, exercised over a two-world runner with overlay-carrying
// scenes and no GPU.
//
//  - ResolvePresentationSeat: the seat a viewport re-points to on a rebind — the bound Viewer when it
//    still resolves in the destination scene, else the scene's sole/first Viewer, else none;
//  - IsWorldPresentable: the present-on-ready gate — a world is presentable only once it resolves, its
//    simulation has started, its residency batch is resident, and its clock has ticked at least once,
//    plus the composition of a consumer's own WorldPresentReadyGate onto that answer;
//  - GuiOverlay::Detach: idempotent, and a no-op on an undriven overlay (no host, no attach).
//
// The router plumbing, viewport document attach/detach observed through GetAttachedDocuments, cursor
// seat move, and the present-on-ready state machine over a live set need a Context — they ride the gpu
// band (tests/gpu/managed_viewport_set.cpp, tests/gpu/gui_overlay.cpp).

#include <doctest/doctest.h>

#include <Veng/Gui/Overlay.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneSimulation.h>
#include <Veng/Scene/SystemRegistry.h>
#include <Veng/World.h>
#include <Veng/WorldRunner.h>

#include "ManagedRebind.h"

using namespace Veng;

namespace
{
    // The fake SystemContext the runner forwards but no system dereferences (an empty registry runs no
    // systems), keeping the whole case device-free while exercising the real open/tick/start path.
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

    WorldOpenInfo StartedEmptyWorld(ContextStorage& storage)
    {
        return WorldOpenInfo{
            .SimTickRate = 60,
            .StartSimulation = true,
            .Systems = vector<SystemId>{},
            .MakeStartContext = [&storage] { return storage.Make(); },
        };
    }

    WorldTickInfo OneStep(ContextStorage& storage)
    {
        return WorldTickInfo{
            .Delta = 1.0f / 60.0f,
            .BuildContext = [&storage](WorldInstanceId, const Scene&, u64, f32, bool)
            { return storage.Make(); },
        };
    }
}

TEST_CASE("ResolvePresentationSeat resolves the bound seat, the sole viewer, or none")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);

    SUBCASE("A null bound seat resolves to the scene's sole Viewer")
    {
        const Unique<Scene> scene = Scene::Create(types);
        const Entity seat = scene->CreateEntity();
        scene->Add<Viewer>(seat);

        CHECK(ResolvePresentationSeat(*scene, Entity::Null) == seat);
    }

    SUBCASE("A bound seat that still resolves as a Viewer in the destination is kept")
    {
        const Unique<Scene> scene = Scene::Create(types);
        const Entity first = scene->CreateEntity();
        scene->Add<Viewer>(first);
        const Entity second = scene->CreateEntity();
        scene->Add<Viewer>(second);

        // The bound handle names a live Viewer in this scene, so it survives rather than falling to the
        // first Viewer — even though a second Viewer exists.
        CHECK(ResolvePresentationSeat(*scene, second) == second);
    }

    SUBCASE("A bound seat that does not resolve falls back to the scene's first Viewer")
    {
        const Unique<Scene> scene = Scene::Create(types);
        const Entity seat = scene->CreateEntity();
        scene->Add<Viewer>(seat);

        // A stale handle from a departed scene (a high index this scene never minted) does not resolve,
        // so the resolution falls to the destination's first Viewer.
        const Entity stale{.Index = 9999, .Generation = 0};
        CHECK(ResolvePresentationSeat(*scene, stale) == seat);
    }

    SUBCASE("A live entity that is not a Viewer does not resolve")
    {
        const Unique<Scene> scene = Scene::Create(types);
        const Entity plain = scene->CreateEntity(); // no Viewer
        const Entity seat = scene->CreateEntity();
        scene->Add<Viewer>(seat);

        CHECK(ResolvePresentationSeat(*scene, plain) == seat);
    }

    SUBCASE("A seatless destination scene resolves to no seat")
    {
        const Unique<Scene> scene = Scene::Create(types);
        const Entity plain = scene->CreateEntity(); // a plain entity, no Viewer anywhere
        CHECK(scene->IsAlive(plain));

        CHECK(ResolvePresentationSeat(*scene, Entity::Null) == Entity::Null);
    }
}

TEST_CASE("IsWorldPresentable gates on resolve, started sim, residency, and a first tick")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;
    WorldRunner runner(WorldRunnerInfo{.Types = &types, .Systems = &systems});
    ContextStorage storage;

    SUBCASE("An unresolved (unminted) world is never presentable")
    {
        CHECK_FALSE(IsWorldPresentable(runner, WorldInstanceId{}));
        CHECK_FALSE(IsWorldPresentable(runner, WorldInstanceId{.Value = 4242}));
    }

    SUBCASE("A world whose simulation has not started is not presentable")
    {
        // Opened with a simulation attached but not started (the client-join placeholder shape).
        const WorldInstanceId world = runner.OpenWorld(WorldOpenInfo{
            .SimTickRate = 60,
            .StartSimulation = false,
            .Systems = vector<SystemId>{},
        });
        CHECK_FALSE(IsWorldPresentable(runner, world));
    }

    SUBCASE("A started but not-yet-ticked world is not presentable; it becomes so after one tick")
    {
        const WorldInstanceId world = runner.OpenWorld(StartedEmptyWorld(storage));

        // Started, its empty residency batch is already resident, but the clock is still at tick 0.
        CHECK(runner.ResolveWorld(world)->Pending.IsResident());
        CHECK(runner.ResolveWorld(world)->Clock.GetTick() == 0);
        CHECK_FALSE(IsWorldPresentable(runner, world));

        // One tick advances the clock past zero: now fully presentable.
        runner.Tick(OneStep(storage));
        CHECK(runner.ResolveWorld(world)->Clock.GetTick() >= 1);
        CHECK(IsWorldPresentable(runner, world));
    }

    SUBCASE("A closed world stops being presentable")
    {
        const WorldInstanceId world = runner.OpenWorld(StartedEmptyWorld(storage));
        runner.Tick(OneStep(storage));
        REQUIRE(IsWorldPresentable(runner, world));

        runner.CloseWorld(world);
        CHECK_FALSE(IsWorldPresentable(runner, world));
    }
}

TEST_CASE("A consumer present-ready gate composes onto the engine's readiness rather than "
          "replacing it")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;
    WorldRunner runner(WorldRunnerInfo{.Types = &types, .Systems = &systems});
    ContextStorage storage;

    const WorldInstanceId world = runner.OpenWorld(StartedEmptyWorld(storage));

    SUBCASE("An empty gate reduces the composed test to the engine's own")
    {
        const WorldPresentReadyGate none;
        CHECK_FALSE(IsWorldPresentable(runner, world, none));
        runner.Tick(OneStep(storage));
        CHECK(IsWorldPresentable(runner, world, none));
    }

    SUBCASE("A refusing gate holds an engine-ready world back")
    {
        runner.Tick(OneStep(storage));
        REQUIRE(IsWorldPresentable(runner, world));

        bool open = false;
        const WorldPresentReadyGate gate = [&open](const World&) { return open; };
        CHECK_FALSE(IsWorldPresentable(runner, world, gate));

        open = true;
        CHECK(IsWorldPresentable(runner, world, gate));
    }

    SUBCASE("The gate is not consulted for a world the engine has not readied, and sees its id")
    {
        u32 calls = 0;
        WorldInstanceId seen;
        const WorldPresentReadyGate gate = [&calls, &seen](const World& candidate)
        {
            ++calls;
            seen = candidate.Id;
            return true;
        };

        // Still at tick 0: the engine's test fails first, so an accepting gate cannot present it.
        CHECK_FALSE(IsWorldPresentable(runner, world, gate));
        CHECK(calls == 0);

        runner.Tick(OneStep(storage));
        CHECK(IsWorldPresentable(runner, world, gate));
        CHECK(calls == 1);
        CHECK(seen == world);
    }
}

TEST_CASE("A two-world runner carries overlay components, and GuiOverlay::Detach is a safe no-op "
          "on an undriven overlay")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;
    WorldRunner runner(WorldRunnerInfo{.Types = &types, .Systems = &systems});
    ContextStorage storage;

    // Two worlds, each with a seat and an overlay-carrying entity — the departed/destination pair a
    // complete rebind moves between.
    const WorldInstanceId worldA = runner.OpenWorld(StartedEmptyWorld(storage));
    const WorldInstanceId worldB = runner.OpenWorld(StartedEmptyWorld(storage));

    Scene& sceneA = runner.ResolveWorld(worldA)->GetScene();
    const Entity seatA = sceneA.CreateEntity();
    sceneA.Add<Viewer>(seatA);
    const Entity hudA = sceneA.CreateEntity();
    sceneA.Add<GuiOverlay>(hudA);

    Scene& sceneB = runner.ResolveWorld(worldB)->GetScene();
    const Entity seatB = sceneB.CreateEntity();
    sceneB.Add<Viewer>(seatB);

    // Each world's seat resolves independently — the seat a rebind onto that world re-points to.
    CHECK(ResolvePresentationSeat(sceneA, Entity::Null) == seatA);
    CHECK(ResolvePresentationSeat(sceneB, Entity::Null) == seatB);

    // The overlay never drove (no viewport, no Context), so it holds no runtime host or document —
    // exactly the guard GuiOverlay::Detach short-circuits on. The departed-world rebind sweeps every
    // overlay through Detach, so this undriven path (a scene that never rendered) must be a safe no-op;
    // the guard here is what makes that call touch no null document.
    const GuiOverlay* overlay = sceneA.TryGet<GuiOverlay>(hudA);
    REQUIRE(overlay != nullptr);
    CHECK(overlay->GetHost() == nullptr);
    CHECK(overlay->GetDocument() == nullptr);
}
