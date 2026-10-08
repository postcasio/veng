// WorldRunner: the flat-peer world scheduler. Opens empty-scene worlds, ticks them independently by
// handle, resolves each by id, closes one without disturbing peers, and honors the pause refcount
// (an explicit toggle plus nested RAII PauseScopes). Pure CPU — device-free by construction: the
// runner is built with only a TypeRegistry + SystemRegistry (no AssetManager, no Context), so the
// empty-scene path spawns and drives worlds with no GPU.
//
// Every runner takes its contexts from a TestServices bundle installed as its context factory: real,
// device-free services, so the whole test exercises the real Tick path with no GPU.

#include <doctest/doctest.h>

#include <algorithm>
#include <array>
#include <cstring>
#include <iterator>
#include <map>
#include <utility>
#include <vector>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Audio/AudioClip.h>
#include <Veng/Audio/AudioEngine.h>
#include <Veng/Localization/Localization.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneSimulation.h>
#include <Veng/Scene/SceneSystem.h>
#include <Veng/Scene/SystemRegistry.h>
#include <Veng/World.h>
#include <Veng/WorldRunner.h>

#include "support/TestServices.h"

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

    // A probe recording how many times its OnStop (end-play) ran per scene, so a close path that
    // must run OnStop exactly once — never zero, never twice — is checkable by handle, and which
    // world each stop's context named, in stop order.
    //
    // Cascade scripts the one case where end-play itself closes a world: set to a live handle, the
    // first OnStop to run closes it and clears the script, so the deferred drain is asked to honour
    // a close issued from inside itself. Unset in every other case.
    struct StopProbe final : SceneSystem
    {
        static inline std::map<const Scene*, int> Stops;
        static inline vector<WorldInstanceId> StoppedWorlds;
        static inline WorldRunner* Runner = nullptr;
        static inline WorldInstanceId Cascade;

        static void Reset()
        {
            Stops.clear();
            StoppedWorlds.clear();
            Runner = nullptr;
            Cascade = WorldInstanceId{};
        }

        void OnUpdate(Scene&, f32, const SystemContext&) override {}
        void OnStop(Scene& scene, const SystemContext& context) override
        {
            ++Stops[&scene];
            StoppedWorlds.push_back(context.World);
            if (Runner != nullptr && Cascade.IsValid())
            {
                const WorldInstanceId target = Cascade;
                Cascade = WorldInstanceId{};
                Runner->CloseWorld(target);
            }
        }
    };

    // A probe recording which world each OnStart's context named, in start order.
    struct StartProbe final : SceneSystem
    {
        static inline vector<WorldInstanceId> StartedWorlds;

        static void Reset() { StartedWorlds.clear(); }

        void OnStart(Scene&, const SystemContext& context) override
        {
            StartedWorlds.push_back(context.World);
        }
        void OnUpdate(Scene&, f32, const SystemContext&) override {}
    };

    // A Sim-phase probe that drives the runner from inside its own update — the reentrancy the
    // deferral exists for. Target scripts which world each scene's update closes (its own, or a
    // peer) and Calls how many times it asks; the probe records what it saw while it ran, so the
    // during-the-tick half of the contract is checkable and not only the after-the-tick half.
    struct CloseProbe final : SceneSystem
    {
        static inline WorldRunner* Runner = nullptr;
        static inline std::map<const Scene*, WorldInstanceId> Target;
        static inline int Calls = 1;
        static inline std::map<const Scene*, int> Updates;
        static inline std::map<const Scene*, bool> ResolvedAfterClose;
        static inline std::map<const Scene*, bool> TickingSeen;

        static void Reset()
        {
            Runner = nullptr;
            Target.clear();
            Calls = 1;
            Updates.clear();
            ResolvedAfterClose.clear();
            TickingSeen.clear();
        }

        void OnUpdate(Scene& scene, f32, const SystemContext&) override
        {
            ++Updates[&scene];
            TickingSeen[&scene] = Runner != nullptr && Runner->IsTicking();
            const auto it = Target.find(&scene);
            if (it == Target.end() || Runner == nullptr)
            {
                return;
            }
            for (int call = 0; call < Calls; ++call)
            {
                Runner->CloseWorld(it->second);
            }
            ResolvedAfterClose[&scene] = Runner->ResolveWorld(it->second) != nullptr;
        }
    };

    // A View-phase probe counting its scene's View passes, so "the closed world took no further
    // phase this frame" is a checked claim rather than an inference from the Sim count.
    struct ViewProbe final : SceneSystem
    {
        static inline std::map<const Scene*, int> Views;

        static void Reset() { Views.clear(); }

        [[nodiscard]] Phase GetPhase() const override { return Phase::View; }
        void OnUpdate(Scene& scene, f32, const SystemContext&) override { ++Views[&scene]; }
    };

    // A Sim-phase probe that opens a world from inside its own update, once, recording whether the
    // fresh handle resolved to a live world before it returned.
    struct OpenProbe final : SceneSystem
    {
        static inline WorldRunner* Runner = nullptr;
        static inline function<WorldOpenInfo()> Open;
        static inline WorldInstanceId Opened;
        static inline bool ResolvedInside = false;

        static void Reset()
        {
            Runner = nullptr;
            Open = {};
            Opened = WorldInstanceId{};
            ResolvedInside = false;
        }

        void OnUpdate(Scene&, f32, const SystemContext&) override
        {
            if (Runner == nullptr || !Open || Opened.IsValid())
            {
                return;
            }
            Opened = Runner->OpenWorld(Open());
            ResolvedInside = Runner->ResolveWorld(Opened) != nullptr;
        }
    };
}

namespace
{
    // A Sim-phase probe placing every Transform at x = tick², so the pose a frame interpolates names
    // the two ticks its history recorded.
    struct MotionProbe final : SceneSystem
    {
        void OnUpdate(Scene& scene, f32, const SystemContext& context) override
        {
            const f32 tick = static_cast<f32>(context.Tick);
            for (auto [entity, transform] : scene.View<Transform>())
            {
                transform.Position = vec3(tick * tick, 0.0f, 0.0f);
            }
        }
    };

    // A Sim-phase probe declaring the tick policy its tag is reset to, recording the tick and the
    // delta of every step it runs on.
    template <int Tag>
    struct PolicyProbe final : SceneSystem
    {
        static inline TickPolicy Policy;
        static inline vector<std::pair<u64, f32>> Runs;

        static void Reset(const TickPolicy policy)
        {
            Policy = policy;
            Runs.clear();
        }

        [[nodiscard]] TickPolicy GetTickPolicy() const override { return Policy; }
        void OnUpdate(Scene&, const f32 delta, const SystemContext& context) override
        {
            Runs.emplace_back(context.Tick, delta);
        }
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

    template <>
    struct VengSystem<StopProbe>
    {
        static constexpr SystemId Id = 0x0071D000000000A3ULL;
        static string Name() { return "StopProbe"; }
    };

    template <>
    struct VengSystem<StartProbe>
    {
        static constexpr SystemId Id = 0xE398360503FCFCCFULL;
        static string Name() { return "StartProbe"; }
    };

    template <>
    struct VengSystem<CloseProbe>
    {
        static constexpr SystemId Id = 0xB762E0308C63F17CULL;
        static string Name() { return "CloseProbe"; }
    };

    template <>
    struct VengSystem<ViewProbe>
    {
        static constexpr SystemId Id = 0x334CD7ACCB0FDBC7ULL;
        static string Name() { return "ViewProbe"; }
    };

    template <>
    struct VengSystem<OpenProbe>
    {
        static constexpr SystemId Id = 0x27AEAA4E6CCF4A71ULL;
        static string Name() { return "OpenProbe"; }
    };

    template <>
    struct VengSystem<MotionProbe>
    {
        static constexpr SystemId Id = 0x3864A0AC7DE8CDFBULL;
        static string Name() { return "MotionProbe"; }
    };

    template <>
    struct VengSystem<PolicyProbe<0>>
    {
        static constexpr SystemId Id = 0xEFBA282B498C078DULL;
        static string Name() { return "PolicyProbe0"; }
    };

    template <>
    struct VengSystem<PolicyProbe<1>>
    {
        static constexpr SystemId Id = 0x1330E3AA24A7ED37ULL;
        static string Name() { return "PolicyProbe1"; }
    };

    template <>
    struct VengSystem<PolicyProbe<2>>
    {
        static constexpr SystemId Id = 0x7AD882136A4CE064ULL;
        static string Name() { return "PolicyProbe2"; }
    };
}

namespace
{
    // A WorldOpenInfo for an empty-scene world running the opener-named probe — the device-free open
    // the runner supports with no AssetManager.
    WorldOpenInfo EmptyWorld()
    {
        return WorldOpenInfo{
            .SimTickRate = 60,
            .StartSimulation = true,
            .Systems = vector<SystemId>{SystemIdOf<TickProbe>()},
        };
    }

    // A WorldOpenInfo for an empty-scene world running the OnStop-recording probe, so closing it
    // exercises the runner's end-play stop.
    WorldOpenInfo StopWorld()
    {
        return WorldOpenInfo{
            .SimTickRate = 60,
            .StartSimulation = true,
            .Systems = vector<SystemId>{SystemIdOf<StopProbe>()},
        };
    }

    // A WorldOpenInfo for an empty-scene world running exactly the named systems — how the
    // reentrancy cases script one world's system set against another's.
    WorldOpenInfo WorldOf(vector<SystemId> systems)
    {
        return WorldOpenInfo{
            .SimTickRate = 60,
            .StartSimulation = true,
            .Systems = std::move(systems),
        };
    }

    // A tick info folding @p delta into every world.
    WorldTickInfo Frame(const f32 delta)
    {
        return WorldTickInfo{.Delta = delta};
    }

    // A tick info that runs one Sim step per call (delta == the 60 Hz fixed step).
    WorldTickInfo OneStep()
    {
        return WorldTickInfo{.Delta = 1.0f / 60.0f};
    }
}

TEST_CASE("A device-free WorldRunner opens two empty worlds and ticks both, resolved by id")
{
    TickProbe::Reset();

    TypeRegistry types;
    SystemRegistry systems;
    systems.Register<TickProbe>();

    TestSupport::TestServices services;
    // No AssetManager, no Context: the runner is device-free and drives only empty-scene worlds.
    WorldRunner runner(WorldRunnerInfo{
        .Types = &types, .Systems = &systems, .Presentation = &services.GetPresentationScopes()});

    runner.SetContextFactory(services.Factory());
    const WorldInstanceId a = runner.OpenWorld(EmptyWorld());
    const WorldInstanceId b = runner.OpenWorld(EmptyWorld());

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
        runner.Tick(OneStep());
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
    TestSupport::TestServices services;
    WorldRunner runner(WorldRunnerInfo{
        .Types = &types, .Systems = &systems, .Presentation = &services.GetPresentationScopes()});
    runner.SetContextFactory(services.Factory());

    // Named set: exactly TickProbe, though the registry also holds OtherProbe.
    const WorldInstanceId named = runner.OpenWorld(WorldOpenInfo{
        .SimTickRate = 60,
        .StartSimulation = true,
        .Systems = vector<SystemId>{SystemIdOf<TickProbe>()},
    });

    // Empty set: a simulation running no systems — the world still starts and its clock ticks
    // (the data-world shape: content arrives by other means, no system advances it).
    const WorldInstanceId bare = runner.OpenWorld(WorldOpenInfo{
        .SimTickRate = 60,
        .StartSimulation = true,
        .Systems = vector<SystemId>{},
    });

    // Disengaged: no simulation at all — the world holds a scene but never ticks.
    const WorldInstanceId simless = runner.OpenWorld(WorldOpenInfo{
        .SimTickRate = 60,
        .StartSimulation = true,
    });

    const Scene* namedScene = &runner.ResolveWorld(named)->GetScene();
    const Scene* bareScene = &runner.ResolveWorld(bare)->GetScene();
    CHECK(runner.ResolveWorld(simless)->GetScene().GetSimulation() == nullptr);

    for (int i = 0; i < 3; ++i)
    {
        runner.Tick(OneStep());
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
    TestSupport::TestServices services;
    WorldRunner runner(WorldRunnerInfo{
        .Types = &types, .Systems = &systems, .Presentation = &services.GetPresentationScopes()});

    runner.SetContextFactory(services.Factory());
    const WorldInstanceId a = runner.OpenWorld(EmptyWorld());
    const WorldInstanceId b = runner.OpenWorld(EmptyWorld());

    const Scene* sceneB = &runner.ResolveWorld(b)->GetScene();

    runner.CloseWorld(a);

    // The closed id resolves to nothing; the peer is untouched and still resolves.
    CHECK(runner.ResolveWorld(a) == nullptr);
    REQUIRE(runner.ResolveWorld(b) != nullptr);

    for (int i = 0; i < 2; ++i)
    {
        runner.Tick(OneStep());
    }

    // Only the surviving world ticked (the closed world's scene was destroyed).
    CHECK(TickProbe::Updates[sceneB] == 2);
    CHECK(runner.ResolveWorld(b)->Clock.GetTick() == 2);
}

TEST_CASE("Closing a runner-started world runs its systems' OnStop exactly once")
{
    StopProbe::Reset();

    TypeRegistry types;
    SystemRegistry systems;
    systems.Register<StopProbe>();
    TestSupport::TestServices services;
    WorldRunner runner(WorldRunnerInfo{
        .Types = &types, .Systems = &systems, .Presentation = &services.GetPresentationScopes()});

    runner.SetContextFactory(services.Factory());
    const WorldInstanceId a = runner.OpenWorld(StopWorld());
    const Scene* scene = &runner.ResolveWorld(a)->GetScene();

    // A started, unclosed world has not run end-play yet.
    CHECK(StopProbe::Stops.find(scene) == StopProbe::Stops.end());

    runner.CloseWorld(a);

    // Closing builds the stop context from the runner's factory and stops the simulation before
    // dropping the world, so OnStop ran once for this scene — the contract a system releasing a
    // resource in OnStop depends on. The id then resolves to nothing.
    CHECK(StopProbe::Stops[scene] == 1);
    CHECK(runner.ResolveWorld(a) == nullptr);
}

TEST_CASE("A world opened over a handed scene starts, ticks and stops as any world does")
{
    StartProbe::Reset();
    StopProbe::Reset();
    OtherProbe::Reset();

    TypeRegistry types;
    SystemRegistry systems;
    systems.Register<StartProbe>();
    systems.Register<StopProbe>();
    systems.Register<OtherProbe>();
    TestSupport::TestServices services;
    WorldRunner runner(WorldRunnerInfo{
        .Types = &types, .Systems = &systems, .Presentation = &services.GetPresentationScopes()});
    runner.SetContextFactory(services.Factory());

    // A scene its caller built, carrying an entity and a simulation of its own that the opener's
    // system set replaces.
    Unique<Scene> built = Scene::Create(types);
    const Entity authored = built->CreateEntity();
    built->SetSimulation(
        CreateUnique<SceneSimulation>(systems, vector<SystemId>{SystemIdOf<OtherProbe>()}));
    const Scene* handed = built.get();

    const WorldInstanceId world = runner.OpenWorld(
        WorldOpenInfo{
            .SimTickRate = 60,
            .StartSimulation = true,
            .Systems = vector<SystemId>{SystemIdOf<StartProbe>(), SystemIdOf<StopProbe>()},
        },
        std::move(built));

    // The world owns the very scene it was handed, and its start named the minted id.
    const World* opened = runner.ResolveWorld(world);
    REQUIRE(opened != nullptr);
    CHECK(&opened->GetScene() == handed);
    CHECK(opened->GetScene().IsAlive(authored));
    CHECK(StartProbe::StartedWorlds == vector<WorldInstanceId>{world});

    // One frame of several steps advances the scene's change tick with the world's clock, which a
    // simulation stepped outside the runner never did.
    runner.Tick(Frame(4.5f / 60.0f));
    CHECK(opened->Clock.GetTick() == 4);
    CHECK(opened->GetScene().GetChangeTick() == opened->Clock.GetTick());
    CHECK(OtherProbe::Updates.empty());

    runner.CloseWorld(world);
    CHECK(StopProbe::Stops[handed] == 1);
    CHECK(StopProbe::StoppedWorlds == vector<WorldInstanceId>{world});
}

TEST_CASE("Closing an externally-started world runs its systems' OnStop exactly once")
{
    StopProbe::Reset();

    TypeRegistry types;
    SystemRegistry systems;
    systems.Register<StopProbe>();
    TestSupport::TestServices services;
    WorldRunner runner(WorldRunnerInfo{
        .Types = &types, .Systems = &systems, .Presentation = &services.GetPresentationScopes()});

    runner.SetContextFactory(services.Factory());

    // The join/travel shape: open a world deferred, install a scene carrying a simulation, then start
    // it externally through Scene::StartSimulation — never the runner's own start.
    const WorldInstanceId a = runner.OpenWorld(WorldOpenInfo{
        .SimTickRate = 60,
        .StartSimulation = false,
    });
    Unique<Scene> owned = Scene::Create(types);
    owned->SetSimulation(
        CreateUnique<SceneSimulation>(systems, vector<SystemId>{SystemIdOf<StopProbe>()}));
    Scene& scene = runner.InstallScene(a, std::move(owned));
    const Scene* key = &scene;
    scene.StartSimulation(services.Make());

    // A started, unclosed world has not run end-play yet.
    CHECK(StopProbe::Stops.find(key) == StopProbe::Stops.end());

    runner.CloseWorld(a);

    // Closing runs OnStop once for the externally-started world, from the runner-level factory — the
    // same guarantee a runner-started world gets, regardless of how the simulation was started.
    CHECK(StopProbe::Stops[key] == 1);
    CHECK(runner.ResolveWorld(a) == nullptr);
}

TEST_CASE("A world stopped before closing runs OnStop once, not twice")
{
    StopProbe::Reset();

    TypeRegistry types;
    SystemRegistry systems;
    systems.Register<StopProbe>();
    TestSupport::TestServices services;
    WorldRunner runner(WorldRunnerInfo{
        .Types = &types, .Systems = &systems, .Presentation = &services.GetPresentationScopes()});

    runner.SetContextFactory(services.Factory());
    const WorldInstanceId a = runner.OpenWorld(StopWorld());
    Scene& scene = runner.ResolveWorld(a)->GetScene();
    const Scene* key = &scene;

    // Stop the simulation by hand first — the overlay-close idiom, which stops while the scene is
    // live and then closes the world. This runs OnStop once.
    scene.StopSimulation(services.Make());
    CHECK(StopProbe::Stops[key] == 1);

    // CloseWorld sees the already-stopped simulation and does not run OnStop again (Stop is
    // idempotent), so end-play runs exactly once across the two stops, never twice.
    runner.CloseWorld(a);
    CHECK(StopProbe::Stops[key] == 1);
    CHECK(runner.ResolveWorld(a) == nullptr);
}

TEST_CASE("Installing over a started scene stops it once, under the world's id")
{
    StopProbe::Reset();

    TypeRegistry types;
    SystemRegistry systems;
    systems.Register<StopProbe>();
    TestSupport::TestServices services;
    WorldRunner runner(WorldRunnerInfo{
        .Types = &types, .Systems = &systems, .Presentation = &services.GetPresentationScopes()});

    runner.SetContextFactory(services.Factory());
    const WorldInstanceId a = runner.OpenWorld(StopWorld());
    const Scene* replaced = &runner.ResolveWorld(a)->GetScene();

    runner.InstallScene(a, Scene::Create(types));

    // The replaced scene ended play exactly once, with the context naming its world; the world stays
    // open on the installed scene.
    CHECK(StopProbe::Stops[replaced] == 1);
    CHECK(StopProbe::StoppedWorlds == vector<WorldInstanceId>{a});
    CHECK(runner.ResolveWorld(a) != nullptr);
}

TEST_CASE("A stopped world holds still, and a later install or close runs no second OnStop")
{
    StopProbe::Reset();
    TickProbe::Reset();

    TypeRegistry types;
    SystemRegistry systems;
    systems.Register<StopProbe>();
    systems.Register<TickProbe>();
    TestSupport::TestServices services;
    WorldRunner runner(WorldRunnerInfo{
        .Types = &types, .Systems = &systems, .Presentation = &services.GetPresentationScopes()});

    runner.SetContextFactory(services.Factory());
    const WorldInstanceId closing =
        runner.OpenWorld(WorldOf({SystemIdOf<StopProbe>(), SystemIdOf<TickProbe>()}));
    const WorldInstanceId installing = runner.OpenWorld(StopWorld());
    const Scene* closingScene = &runner.ResolveWorld(closing)->GetScene();
    const Scene* installingScene = &runner.ResolveWorld(installing)->GetScene();
    runner.Tick(OneStep());
    REQUIRE(TickProbe::Updates[closingScene] == 1);

    runner.StopWorld(closing);
    runner.StopWorld(closing);
    runner.StopWorld(installing);
    CHECK(StopProbe::Stops[closingScene] == 1);
    CHECK(StopProbe::Stops[installingScene] == 1);

    // Open but unticked: no phase runs and the clock stands.
    runner.Tick(OneStep());
    REQUIRE(runner.ResolveWorld(closing) != nullptr);
    CHECK(TickProbe::Updates[closingScene] == 1);
    CHECK(runner.ResolveWorld(closing)->Clock.GetTick() == 1);

    runner.InstallScene(installing, Scene::Create(types));
    runner.CloseWorld(closing);
    CHECK(StopProbe::Stops[closingScene] == 1);
    CHECK(StopProbe::Stops[installingScene] == 1);
}

TEST_CASE("Closing every world stops each once, newest first, and tells the closed hook after each")
{
    StopProbe::Reset();

    TypeRegistry types;
    SystemRegistry systems;
    systems.Register<StopProbe>();
    TestSupport::TestServices services;
    WorldRunner runner(WorldRunnerInfo{
        .Types = &types, .Systems = &systems, .Presentation = &services.GetPresentationScopes()});

    runner.SetContextFactory(services.Factory());

    // Each call records the id and whether it still resolved: a hook fired before the erase would
    // hand a holder an id that still names a live world.
    vector<std::pair<WorldInstanceId, bool>> closed;
    runner.SetWorldClosedHook(
        [&](const WorldInstanceId world)
        { closed.emplace_back(world, runner.ResolveWorld(world) != nullptr); });

    const WorldInstanceId a = runner.OpenWorld(StopWorld());
    const WorldInstanceId b = runner.OpenWorld(StopWorld());
    const WorldInstanceId c = runner.OpenWorld(StopWorld());

    // Replacing a scene closes no world, so it is not reported.
    runner.InstallScene(b, Scene::Create(types));
    CHECK(closed.empty());

    runner.CloseAllWorlds();

    // b's replaced scene stopped at the install; the sweep stops c, then a (b's installed scene runs
    // no system), and reports all three closes newest first, each once erased.
    CHECK(StopProbe::StoppedWorlds == vector<WorldInstanceId>{b, c, a});
    CHECK(closed == vector<std::pair<WorldInstanceId, bool>>{{c, false}, {b, false}, {a, false}});
    CHECK_FALSE(runner.HasWorlds());
}

TEST_CASE("A runner with no context factory closes a started world without running OnStop")
{
    StopProbe::Reset();

    TypeRegistry types;
    SystemRegistry systems;
    systems.Register<StopProbe>();
    TestSupport::TestServices services;
    WorldRunner runner(WorldRunnerInfo{
        .Types = &types, .Systems = &systems, .Presentation = &services.GetPresentationScopes()});

    runner.SetContextFactory(services.Factory());
    const WorldInstanceId a = runner.OpenWorld(StopWorld());
    const Scene* scene = &runner.ResolveWorld(a)->GetScene();

    // The device-free contract: a runner with no factory drops a started world without fabricating a
    // context, so OnStop simply does not run.
    runner.SetContextFactory({});
    CHECK_FALSE(runner.HasContextFactory());
    runner.CloseWorld(a);

    // The started world closed cleanly and resolves to nothing; end-play never ran, since there was
    // no context to run it with.
    CHECK(StopProbe::Stops.find(scene) == StopProbe::Stops.end());
    CHECK(runner.ResolveWorld(a) == nullptr);
}

TEST_CASE("A paused world's sim does not advance while a peer's does")
{
    TickProbe::Reset();

    TypeRegistry types;
    SystemRegistry systems;
    systems.Register<TickProbe>();
    TestSupport::TestServices services;
    WorldRunner runner(WorldRunnerInfo{
        .Types = &types, .Systems = &systems, .Presentation = &services.GetPresentationScopes()});

    runner.SetContextFactory(services.Factory());
    const WorldInstanceId a = runner.OpenWorld(EmptyWorld());
    const WorldInstanceId b = runner.OpenWorld(EmptyWorld());

    const Scene* sceneA = &runner.ResolveWorld(a)->GetScene();
    const Scene* sceneB = &runner.ResolveWorld(b)->GetScene();

    runner.SetWorldPaused(a, true);
    CHECK(runner.IsWorldPaused(a));
    CHECK_FALSE(runner.IsWorldPaused(b));

    for (int i = 0; i < 3; ++i)
    {
        runner.Tick(OneStep());
    }

    // The paused world ran no Sim step and its tick held; the peer advanced normally.
    CHECK(TickProbe::Updates.find(sceneA) == TickProbe::Updates.end());
    CHECK(runner.ResolveWorld(a)->Clock.GetTick() == 0);
    CHECK(TickProbe::Updates[sceneB] == 3);
    CHECK(runner.ResolveWorld(b)->Clock.GetTick() == 3);

    // Resuming ticks it again, and — because a paused world drops its accumulator — it chases no
    // backlog for the frames it sat paused.
    runner.SetWorldPaused(a, false);
    runner.Tick(OneStep());
    CHECK(TickProbe::Updates[sceneA] == 1);
    CHECK(runner.ResolveWorld(a)->Clock.GetTick() == 1);
}

TEST_CASE("Two pause scopes and the toggle hold one pause the scene sees, released in any order")
{
    TypeRegistry types;
    SystemRegistry systems;
    systems.Register<TickProbe>();
    TestSupport::TestServices services;
    WorldRunner runner(WorldRunnerInfo{
        .Types = &types, .Systems = &systems, .Presentation = &services.GetPresentationScopes()});

    runner.SetContextFactory(services.Factory());
    const WorldInstanceId a = runner.OpenWorld(EmptyWorld());
    const Scene& scene = runner.ResolveWorld(a)->GetScene();

    // Holder 0 and 1 are scopes, 2 the explicit toggle; every release order is its own claim.
    std::array<int, 3> order{0, 1, 2};
    do
    {
        std::array<WorldPauseScope, 2> scopes{runner.PauseScope(a), runner.PauseScope(a)};
        runner.SetWorldPaused(a, true);
        for (usize i = 0; i < order.size(); ++i)
        {
            CHECK(scene.IsSimulationPaused());
            if (order[i] == 2)
            {
                runner.SetWorldPaused(a, false);
            }
            else
            {
                scopes[static_cast<usize>(order[i])] = WorldPauseScope{};
            }
        }
        CHECK_FALSE(scene.IsSimulationPaused());
        CHECK_FALSE(runner.IsWorldPaused(a));
    } while (std::ranges::next_permutation(order).found);
}

TEST_CASE("A pause scope outliving its world, or opened on one with no simulation, holds nothing")
{
    TypeRegistry types;
    SystemRegistry systems;
    systems.Register<TickProbe>();
    TestSupport::TestServices services;
    WorldRunner runner(WorldRunnerInfo{
        .Types = &types, .Systems = &systems, .Presentation = &services.GetPresentationScopes()});

    runner.SetContextFactory(services.Factory());
    const WorldInstanceId closing = runner.OpenWorld(EmptyWorld());
    const WorldInstanceId peer = runner.OpenWorld(EmptyWorld());

    WorldPauseScope outlived = runner.PauseScope(closing);
    REQUIRE(outlived.IsHeld());
    runner.CloseWorld(closing);
    outlived = WorldPauseScope{};
    CHECK_FALSE(runner.IsWorldPaused(peer));

    // A world with no simulation never ticks, so there is nothing for a pause to hold.
    const WorldInstanceId bare = runner.OpenWorld(WorldOpenInfo{.StartSimulation = false});
    CHECK_FALSE(runner.PauseScope(bare).IsHeld());
    runner.SetWorldPaused(bare, true);
    CHECK_FALSE(runner.IsWorldPaused(bare));
}

TEST_CASE("A paused world runs neither its Sim nor its View systems")
{
    TickProbe::Reset();
    ViewProbe::Reset();

    TypeRegistry types;
    SystemRegistry systems;
    systems.Register<TickProbe>();
    systems.Register<ViewProbe>();
    TestSupport::TestServices services;
    WorldRunner runner(WorldRunnerInfo{
        .Types = &types, .Systems = &systems, .Presentation = &services.GetPresentationScopes()});

    runner.SetContextFactory(services.Factory());
    const WorldInstanceId a =
        runner.OpenWorld(WorldOf({SystemIdOf<TickProbe>(), SystemIdOf<ViewProbe>()}));
    const Scene* scene = &runner.ResolveWorld(a)->GetScene();

    // A frame long enough for several steps.
    const WorldPauseScope pause = runner.PauseScope(a);
    runner.Tick(Frame(3.5f / 60.0f));
    CHECK(TickProbe::Updates.find(scene) == TickProbe::Updates.end());
    CHECK(ViewProbe::Views.find(scene) == ViewProbe::Views.end());
}

TEST_CASE("A pause held across InstallScene survives it and releases onto the installed scene")
{
    TypeRegistry types;
    SystemRegistry systems;
    systems.Register<TickProbe>();
    TestSupport::TestServices services;
    WorldRunner runner(WorldRunnerInfo{
        .Types = &types, .Systems = &systems, .Presentation = &services.GetPresentationScopes()});

    runner.SetContextFactory(services.Factory());
    const WorldInstanceId a = runner.OpenWorld(EmptyWorld());

    WorldPauseScope scope = runner.PauseScope(a);
    runner.SetWorldPaused(a, true);

    Unique<Scene> replacement = Scene::Create(types);
    replacement->SetSimulation(CreateUnique<SceneSimulation>(systems, vector<SystemId>{}));
    const Scene& installed = runner.InstallScene(a, std::move(replacement));
    CHECK(installed.IsSimulationPaused());

    scope = WorldPauseScope{};
    CHECK(installed.IsSimulationPaused()); // the carried toggle still holds it
    runner.SetWorldPaused(a, false);
    CHECK_FALSE(installed.IsSimulationPaused());
}

TEST_CASE("A system closing its own world from its update stops it after the walk, not during")
{
    CloseProbe::Reset();
    ViewProbe::Reset();
    StopProbe::Reset();

    TypeRegistry types;
    SystemRegistry systems;
    systems.Register<CloseProbe>();
    systems.Register<ViewProbe>();
    systems.Register<StopProbe>();
    TestSupport::TestServices services;
    WorldRunner runner(WorldRunnerInfo{
        .Types = &types, .Systems = &systems, .Presentation = &services.GetPresentationScopes()});

    runner.SetContextFactory(services.Factory());
    CloseProbe::Runner = &runner;

    const WorldInstanceId a = runner.OpenWorld(
        WorldOf({SystemIdOf<CloseProbe>(), SystemIdOf<ViewProbe>(), SystemIdOf<StopProbe>()}));
    const Scene* scene = &runner.ResolveWorld(a)->GetScene();
    CloseProbe::Target[scene] = a;

    runner.Tick(OneStep());

    // The update ran to completion, saw the runner ticking, and still resolved its own world after
    // asking for the close: the scene a system is standing in stays live for the rest of its call.
    CHECK(CloseProbe::Updates[scene] == 1);
    CHECK(CloseProbe::TickingSeen[scene]);
    CHECK(CloseProbe::ResolvedAfterClose[scene]);

    // A queued world takes no further phase this frame — the View pass was skipped.
    CHECK(ViewProbe::Views.find(scene) == ViewProbe::Views.end());

    // End-play ran exactly once, at the drain, and the world is gone by the time Tick returns.
    CHECK(StopProbe::Stops[scene] == 1);
    CHECK(runner.ResolveWorld(a) == nullptr);
    CHECK_FALSE(runner.IsTicking());
}

TEST_CASE("A world closed from an earlier world's update takes no phase at all that frame")
{
    CloseProbe::Reset();
    ViewProbe::Reset();

    TypeRegistry types;
    SystemRegistry systems;
    systems.Register<CloseProbe>();
    systems.Register<ViewProbe>();
    TestSupport::TestServices services;
    WorldRunner runner(WorldRunnerInfo{
        .Types = &types, .Systems = &systems, .Presentation = &services.GetPresentationScopes()});

    runner.SetContextFactory(services.Factory());
    CloseProbe::Runner = &runner;

    // The closer is opened first, so its victim sits later in the walk and is reached after the
    // close is issued — the ordering where an immediate erase would have moved the walk's footing.
    const WorldInstanceId closer = runner.OpenWorld(WorldOf({SystemIdOf<CloseProbe>()}));
    const WorldInstanceId victim =
        runner.OpenWorld(WorldOf({SystemIdOf<CloseProbe>(), SystemIdOf<ViewProbe>()}));
    const Scene* closerScene = &runner.ResolveWorld(closer)->GetScene();
    const Scene* victimScene = &runner.ResolveWorld(victim)->GetScene();
    CloseProbe::Target[closerScene] = victim;

    runner.Tick(OneStep());

    // The closer ticked normally; the victim ran neither a Sim step nor a View pass and its clock
    // never advanced, then went away with the walk.
    CHECK(CloseProbe::Updates[closerScene] == 1);
    CHECK(CloseProbe::Updates.find(victimScene) == CloseProbe::Updates.end());
    CHECK(ViewProbe::Views.find(victimScene) == ViewProbe::Views.end());
    CHECK(runner.ResolveWorld(victim) == nullptr);
    REQUIRE(runner.ResolveWorld(closer) != nullptr);
    CHECK(runner.ResolveWorld(closer)->Clock.GetTick() == 1);
}

TEST_CASE("A world opened from inside a tick is live at once and first ticks the next frame")
{
    OpenProbe::Reset();
    TickProbe::Reset();

    TypeRegistry types;
    SystemRegistry systems;
    systems.Register<OpenProbe>();
    systems.Register<TickProbe>();
    TestSupport::TestServices services;
    WorldRunner runner(WorldRunnerInfo{
        .Types = &types, .Systems = &systems, .Presentation = &services.GetPresentationScopes()});

    runner.SetContextFactory(services.Factory());
    OpenProbe::Runner = &runner;
    OpenProbe::Open = [] { return WorldOf({SystemIdOf<TickProbe>()}); };

    const WorldInstanceId opener =
        runner.OpenWorld(WorldOf({SystemIdOf<OpenProbe>(), SystemIdOf<TickProbe>()}));
    const Scene* openerScene = &runner.ResolveWorld(opener)->GetScene();

    runner.Tick(OneStep());

    // The open landed at once: the system held a valid handle resolving to a live world before its
    // update returned.
    REQUIRE(OpenProbe::Opened.IsValid());
    CHECK(OpenProbe::ResolvedInside);
    REQUIRE(runner.ResolveWorld(OpenProbe::Opened) != nullptr);
    const Scene* openedScene = &runner.ResolveWorld(OpenProbe::Opened)->GetScene();

    // But it is not ticked by the walk that opened it — the walk runs over the count it captured.
    CHECK(TickProbe::Updates.find(openedScene) == TickProbe::Updates.end());
    CHECK(runner.ResolveWorld(OpenProbe::Opened)->Clock.GetTick() == 0);

    runner.Tick(OneStep());

    // The next frame is its first: one step to the opener's two.
    CHECK(TickProbe::Updates[openedScene] == 1);
    CHECK(TickProbe::Updates[openerScene] == 2);
}

TEST_CASE("An open that reallocates the world vector leaves the opening world's tick intact")
{
    OpenProbe::Reset();
    TickProbe::Reset();
    ViewProbe::Reset();

    TypeRegistry types;
    SystemRegistry systems;
    systems.Register<OpenProbe>();
    systems.Register<TickProbe>();
    systems.Register<ViewProbe>();
    TestSupport::TestServices services;
    WorldRunner runner(WorldRunnerInfo{
        .Types = &types, .Systems = &systems, .Presentation = &services.GetPresentationScopes()});

    runner.SetContextFactory(services.Factory());
    OpenProbe::Runner = &runner;
    OpenProbe::Open = [] { return WorldOf({SystemIdOf<TickProbe>()}); };

    const WorldInstanceId opener = runner.OpenWorld(
        WorldOf({SystemIdOf<OpenProbe>(), SystemIdOf<TickProbe>(), SystemIdOf<ViewProbe>()}));
    const Scene* openerScene = &runner.ResolveWorld(opener)->GetScene();

    // Fill the world vector to exactly its capacity, so the open issued from inside the tick is
    // certain to reallocate and move every slot — the case a reference into the vector would not
    // survive, while the heap World the walk holds does.
    while (runner.GetWorlds().size() < runner.GetWorlds().capacity())
    {
        (void)runner.OpenWorld(EmptyWorld());
    }
    const Unique<World>* const slotsBefore = runner.GetWorlds().data();

    runner.Tick(OneStep());

    CHECK(runner.GetWorlds().data() != slotsBefore);

    // The opening world finished its own tick across the reallocation: its Sim step ran, its View
    // pass ran, and its clock — read from the World after the append — advanced.
    CHECK(TickProbe::Updates[openerScene] == 1);
    CHECK(ViewProbe::Views[openerScene] == 1);
    REQUIRE(runner.ResolveWorld(opener) != nullptr);
    CHECK(runner.ResolveWorld(opener)->Clock.GetTick() == 1);
}

TEST_CASE("Two closes of one world within a tick close it once")
{
    CloseProbe::Reset();
    StopProbe::Reset();

    TypeRegistry types;
    SystemRegistry systems;
    systems.Register<CloseProbe>();
    systems.Register<StopProbe>();
    TestSupport::TestServices services;
    WorldRunner runner(WorldRunnerInfo{
        .Types = &types, .Systems = &systems, .Presentation = &services.GetPresentationScopes()});

    runner.SetContextFactory(services.Factory());
    CloseProbe::Runner = &runner;
    CloseProbe::Calls = 2;

    const WorldInstanceId a =
        runner.OpenWorld(WorldOf({SystemIdOf<CloseProbe>(), SystemIdOf<StopProbe>()}));
    const Scene* scene = &runner.ResolveWorld(a)->GetScene();
    CloseProbe::Target[scene] = a;

    runner.Tick(OneStep());

    // The second ask is absorbed: end-play ran once, not twice.
    CHECK(StopProbe::Stops[scene] == 1);
    CHECK(runner.ResolveWorld(a) == nullptr);
}

TEST_CASE("A close issued from a system's OnStop is drained in its turn")
{
    CloseProbe::Reset();
    StopProbe::Reset();

    TypeRegistry types;
    SystemRegistry systems;
    systems.Register<CloseProbe>();
    systems.Register<StopProbe>();
    TestSupport::TestServices services;
    WorldRunner runner(WorldRunnerInfo{
        .Types = &types, .Systems = &systems, .Presentation = &services.GetPresentationScopes()});

    runner.SetContextFactory(services.Factory());
    CloseProbe::Runner = &runner;
    StopProbe::Runner = &runner;

    const WorldInstanceId first =
        runner.OpenWorld(WorldOf({SystemIdOf<CloseProbe>(), SystemIdOf<StopProbe>()}));
    const WorldInstanceId cascaded = runner.OpenWorld(WorldOf({SystemIdOf<StopProbe>()}));
    const Scene* firstScene = &runner.ResolveWorld(first)->GetScene();
    const Scene* cascadedScene = &runner.ResolveWorld(cascaded)->GetScene();

    // The first world closes itself from its update; its end-play, running at the drain, closes the
    // second — a close issued from inside the drain, which the drain must still honour.
    CloseProbe::Target[firstScene] = first;
    StopProbe::Cascade = cascaded;

    runner.Tick(OneStep());

    CHECK(StopProbe::Stops[firstScene] == 1);
    CHECK(StopProbe::Stops[cascadedScene] == 1);
    CHECK(runner.ResolveWorld(first) == nullptr);
    CHECK(runner.ResolveWorld(cascaded) == nullptr);
    CHECK_FALSE(runner.IsTicking());
}

TEST_CASE("The scene-retiring hook names each scene the runner destroys, while its world resolves")
{
    CloseProbe::Reset();

    TypeRegistry types;
    SystemRegistry systems;
    systems.Register<CloseProbe>();
    TestSupport::TestServices services;
    WorldRunner runner(WorldRunnerInfo{
        .Types = &types, .Systems = &systems, .Presentation = &services.GetPresentationScopes()});

    runner.SetContextFactory(services.Factory());
    CloseProbe::Runner = &runner;

    // Each call records the scene and whether its world still resolved: a hook that ran after the
    // erase would be handed a destroyed scene.
    vector<std::pair<const Scene*, bool>> retired;
    WorldInstanceId expected;
    runner.SetSceneRetiringHook(
        [&](const Scene& scene)
        {
            const World* world = runner.ResolveWorld(expected);
            retired.emplace_back(&scene, world != nullptr && &world->GetScene() == &scene);
        });

    const WorldInstanceId immediate = runner.OpenWorld(EmptyWorld());
    const WorldInstanceId queued = runner.OpenWorld(WorldOf({SystemIdOf<CloseProbe>()}));
    const WorldInstanceId installed = runner.OpenWorld(EmptyWorld());
    const WorldInstanceId peer = runner.OpenWorld(EmptyWorld());
    const Scene* immediateScene = &runner.ResolveWorld(immediate)->GetScene();
    const Scene* queuedScene = &runner.ResolveWorld(queued)->GetScene();
    const Scene* placeholder = &runner.ResolveWorld(installed)->GetScene();

    // A close outside a tick retires the scene at the call.
    expected = immediate;
    runner.CloseWorld(immediate);
    REQUIRE(retired.size() == 1);
    CHECK(retired[0] == std::pair{immediateScene, true});

    // A close a system issues inside the tick retires the scene at the drain, once.
    expected = queued;
    CloseProbe::Target[queuedScene] = queued;
    runner.Tick(OneStep());
    REQUIRE(retired.size() == 2);
    CHECK(retired[1] == std::pair{queuedScene, true});

    // Installing over a placeholder retires the placeholder; the installed scene is not retired.
    expected = installed;
    runner.InstallScene(installed, Scene::Create(types));
    REQUIRE(retired.size() == 3);
    CHECK(retired[2] == std::pair{placeholder, true});

    // The peer was never named.
    CHECK(std::ranges::none_of(retired, [&](const auto& entry)
                               { return entry.first == &runner.ResolveWorld(peer)->GetScene(); }));
}

TEST_CASE("A close issued outside a tick applies before the call returns")
{
    StopProbe::Reset();

    TypeRegistry types;
    SystemRegistry systems;
    systems.Register<StopProbe>();
    TestSupport::TestServices services;
    WorldRunner runner(WorldRunnerInfo{
        .Types = &types, .Systems = &systems, .Presentation = &services.GetPresentationScopes()});

    runner.SetContextFactory(services.Factory());
    const WorldInstanceId a = runner.OpenWorld(WorldOf({SystemIdOf<StopProbe>()}));
    const Scene* scene = &runner.ResolveWorld(a)->GetScene();

    // Nothing is ticking, so the deferral does not apply: the world is stopped and gone at the call
    // rather than at some later drain.
    CHECK_FALSE(runner.IsTicking());
    runner.CloseWorld(a);
    CHECK(StopProbe::Stops[scene] == 1);
    CHECK(runner.ResolveWorld(a) == nullptr);
}

TEST_CASE("OpenWorld's MaxTicksPerFrame caps the Sim steps a world runs in one frame")
{
    TickProbe::Reset();

    TypeRegistry types;
    SystemRegistry systems;
    systems.Register<TickProbe>();
    TestSupport::TestServices services;
    WorldRunner runner(WorldRunnerInfo{
        .Types = &types, .Systems = &systems, .Presentation = &services.GetPresentationScopes()});

    runner.SetContextFactory(services.Factory());
    WorldOpenInfo info = EmptyWorld();
    info.MaxTicksPerFrame = 2;
    const WorldInstanceId world = runner.OpenWorld(info);

    // A one-second frame owes sixty steps; the world runs its own cap of two.
    runner.Tick(Frame(1.0f));
    CHECK(TickProbe::Updates[&runner.ResolveWorld(world)->GetScene()] == 2);
    CHECK(runner.ResolveWorld(world)->Clock.GetTick() == 2);
}

TEST_CASE(
    "A world records only a frame's final two poses, and interpolates as a two-step frame does")
{
    TypeRegistry types;
    types.Register<Transform>("Transform");
    SystemRegistry systems;
    systems.Register<MotionProbe>();
    TestSupport::TestServices services;
    constexpr f32 Step = 1.0f / 60.0f;

    // Opens a world holding one Transform the motion probe moves, returning the world and entity.
    const auto open = [&](WorldRunner& runner)
    {
        runner.SetContextFactory(services.Factory());
        Entity entity;
        const WorldInstanceId world = runner.OpenWorld(WorldOpenInfo{
            .SimTickRate = 60,
            .StartSimulation = true,
            .Systems = vector<SystemId>{SystemIdOf<MotionProbe>()},
            .OnLoaded =
                [&entity](WorldInstanceId, Scene& scene, ResidencyBatch&)
            {
                entity = scene.CreateEntity();
                scene.Add<Transform>(entity, Transform{});
            },
        });
        return std::pair{world, entity};
    };

    // Five steps (ticks 1..5) against two steps over the same final ticks (4, 5), both half a step
    // into the next tick.
    WorldRunner fiveRunner(WorldRunnerInfo{
        .Types = &types, .Systems = &systems, .Presentation = &services.GetPresentationScopes()});
    const auto [five, fiveEntity] = open(fiveRunner);
    fiveRunner.Tick(Frame(Step * 5.5f));

    WorldRunner twoRunner(WorldRunnerInfo{
        .Types = &types, .Systems = &systems, .Presentation = &services.GetPresentationScopes()});
    const auto [two, twoEntity] = open(twoRunner);
    twoRunner.ResolveWorld(two)->Clock.SetTick(3);
    twoRunner.Tick(Frame(Step * 2.5f));

    const World& fiveWorld = *fiveRunner.ResolveWorld(five);
    const World& twoWorld = *twoRunner.ResolveWorld(two);
    REQUIRE(fiveWorld.Clock.GetTick() == 5);
    REQUIRE(twoWorld.Clock.GetTick() == 5);
    REQUIRE(fiveWorld.LastAlpha == doctest::Approx(0.5f));
    REQUIRE(twoWorld.LastAlpha == doctest::Approx(0.5f));

    // Both blend tick 4 (x = 16) into tick 5 (x = 25).
    Scene& fiveScene = fiveWorld.GetScene();
    const f32 fiveX = fiveScene.GetInterpolatedWorldTransform(fiveEntity, 0.5f)[3].x;
    const f32 twoX = twoWorld.GetScene().GetInterpolatedWorldTransform(twoEntity, 0.5f)[3].x;
    CHECK(fiveX == doctest::Approx(20.5f));
    CHECK(twoX == doctest::Approx(fiveX));

    // A step told not to record leaves the history where it was; one told to record rolls it.
    SystemContext context = services.Make();
    context.Tick = 6;
    fiveScene.TickSimulationPhase(SceneSystem::Phase::Sim, Step, context, false);
    CHECK(fiveScene.GetInterpolatedWorldTransform(fiveEntity, 0.5f)[3].x == doctest::Approx(20.5f));
    fiveScene.TickSimulationPhase(SceneSystem::Phase::Sim, Step, context, true);
    CHECK(fiveScene.GetInterpolatedWorldTransform(fiveEntity, 0.5f)[3].x == doctest::Approx(30.5f));
}

TEST_CASE("An EveryNth system runs on the ticks its period selects, however frames group the steps")
{
    using TickPolicy = SceneSystem::TickPolicy;
    PolicyProbe<0>::Reset(TickPolicy::EveryNth(4, 1));

    TypeRegistry types;
    SystemRegistry systems;
    systems.Register<PolicyProbe<0>>();
    TestSupport::TestServices services;
    WorldRunner runner(WorldRunnerInfo{
        .Types = &types, .Systems = &systems, .Presentation = &services.GetPresentationScopes()});
    runner.SetContextFactory(services.Factory());
    const WorldInstanceId world = runner.OpenWorld(WorldOf({SystemIdOf<PolicyProbe<0>>()}));
    constexpr f32 Step = 1.0f / 60.0f;

    // Frames of uneven step counts, until sixty ticks have run.
    constexpr f32 Pattern[] = {1.25f, 3.25f, 0.5f, 4.25f, 2.25f};
    for (usize frame = 0; runner.ResolveWorld(world)->Clock.GetTick() < 60; ++frame)
    {
        runner.Tick(Frame(Step * Pattern[frame % std::size(Pattern)]));
    }

    vector<u64> expected;
    for (u64 tick = 1; tick <= runner.ResolveWorld(world)->Clock.GetTick(); tick += 4)
    {
        expected.push_back(tick);
    }
    vector<u64> ran;
    u32 wrongDelta = 0;
    for (const auto& [tick, delta] : PolicyProbe<0>::Runs)
    {
        ran.push_back(tick);
        wrongDelta += delta == doctest::Approx(4.0f * Step) ? 0u : 1u;
    }
    CHECK(ran == expected);
    CHECK(wrongDelta == 0);
}

TEST_CASE("Frame-keyed systems run once per frame and are handed the time since they last ran")
{
    using TickPolicy = SceneSystem::TickPolicy;
    PolicyProbe<1>::Reset(TickPolicy::FirstStepOfFrame());
    PolicyProbe<2>::Reset(TickPolicy::LastStepOfFrame());

    TypeRegistry types;
    SystemRegistry systems;
    systems.Register<PolicyProbe<1>>();
    systems.Register<PolicyProbe<2>>();
    TestSupport::TestServices services;
    WorldRunner runner(WorldRunnerInfo{
        .Types = &types, .Systems = &systems, .Presentation = &services.GetPresentationScopes()});
    runner.SetContextFactory(services.Factory());
    const WorldInstanceId world =
        runner.OpenWorld(WorldOf({SystemIdOf<PolicyProbe<1>>(), SystemIdOf<PolicyProbe<2>>()}));
    constexpr f32 Step = 1.0f / 60.0f;

    // Each stepping frame's first and last tick, read off the clock around it.
    constexpr f32 Pattern[] = {1.25f, 3.25f, 0.5f, 4.25f, 2.25f, 0.25f};
    vector<u64> firstTicks;
    vector<u64> lastTicks;
    for (usize frame = 0; frame < 18; ++frame)
    {
        const u64 before = runner.ResolveWorld(world)->Clock.GetTick();
        runner.Tick(Frame(Step * Pattern[frame % std::size(Pattern)]));
        const u64 after = runner.ResolveWorld(world)->Clock.GetTick();
        if (after > before)
        {
            firstTicks.push_back(before + 1);
            lastTicks.push_back(after);
        }
    }

    vector<u64> firstRan;
    f32 firstTime = 0.0f;
    for (const auto& [tick, delta] : PolicyProbe<1>::Runs)
    {
        firstRan.push_back(tick);
        firstTime += delta;
    }
    vector<u64> lastRan;
    f32 lastTime = 0.0f;
    for (const auto& [tick, delta] : PolicyProbe<2>::Runs)
    {
        lastRan.push_back(tick);
        lastTime += delta;
    }

    CHECK(firstRan == firstTicks);
    CHECK(lastRan == lastTicks);
    // The deltas handed add up to the time simulated through each system's last run.
    REQUIRE_FALSE(firstRan.empty());
    CHECK(firstTime == doctest::Approx(static_cast<f32>(firstRan.back()) * Step));
    CHECK(lastTime == doctest::Approx(static_cast<f32>(lastRan.back()) * Step));
}

namespace
{
    // A Sim-phase probe recording, per scene, the world each lifecycle call's context names, and the
    // delta and replay flag of the last step it ran.
    struct WorldProbe final : SceneSystem
    {
        static inline std::map<const Scene*, WorldInstanceId> Started;
        static inline std::map<const Scene*, WorldInstanceId> Stepped;
        static inline std::map<const Scene*, f32> StepDelta;
        static inline std::map<const Scene*, bool> StepReplay;

        static void Reset()
        {
            Started.clear();
            Stepped.clear();
            StepDelta.clear();
            StepReplay.clear();
        }

        void OnStart(Scene& scene, const SystemContext& context) override
        {
            Started[&scene] = context.World;
        }

        void OnUpdate(Scene& scene, const f32 delta, const SystemContext& context) override
        {
            Stepped[&scene] = context.World;
            StepDelta[&scene] = delta;
            StepReplay[&scene] = context.IsReplay;
        }
    };
}

namespace Veng
{
    template <>
    struct VengSystem<WorldProbe>
    {
        static constexpr SystemId Id = 0xA89FBC5EDE1C2DCBULL;
        static string Name() { return "WorldProbe"; }
    };
}

TEST_CASE("A world's start context names the world, whether it starts at open or deferred")
{
    WorldProbe::Reset();

    TypeRegistry types;
    SystemRegistry systems;
    systems.Register<WorldProbe>();
    TestSupport::TestServices services;
    WorldRunner runner(WorldRunnerInfo{
        .Types = &types, .Systems = &systems, .Presentation = &services.GetPresentationScopes()});
    runner.SetContextFactory(services.Factory());

    const WorldInstanceId atOpen = runner.OpenWorld(WorldOf({SystemIdOf<WorldProbe>()}));
    WorldOpenInfo deferredInfo = WorldOf({SystemIdOf<WorldProbe>()});
    deferredInfo.StartSimulation = false;
    const WorldInstanceId deferred = runner.OpenWorld(deferredInfo);
    const Scene* deferredScene = &runner.ResolveWorld(deferred)->GetScene();
    CHECK(WorldProbe::Started.find(deferredScene) == WorldProbe::Started.end());
    runner.StartWorld(deferred);

    CHECK(WorldProbe::Started[&runner.ResolveWorld(atOpen)->GetScene()] == atOpen);
    CHECK(WorldProbe::Started[deferredScene] == deferred);
    CHECK(runner.ResolveWorld(deferred)->GetScene().GetSimulation()->IsStarted());
}

TEST_CASE("The runner builds every context through its factory, each naming the world")
{
    TypeRegistry types;
    SystemRegistry systems;
    systems.Register<TickProbe>();
    TestSupport::TestServices services;
    WorldRunner runner(WorldRunnerInfo{
        .Types = &types, .Systems = &systems, .Presentation = &services.GetPresentationScopes()});

    std::map<SystemContextPhase, int> counts;
    vector<WorldInstanceId> named;
    int lastSteps = 0;
    runner.SetContextFactory(
        [&](const SystemContextRequest& request)
        {
            ++counts[request.Phase];
            named.push_back(request.World);
            lastSteps += request.LastStep ? 1 : 0;
            return services.Make(request);
        });

    const WorldInstanceId world = runner.OpenWorld(EmptyWorld());
    constexpr int Steps = 3;
    runner.Tick(Frame(static_cast<f32>(Steps) / 60.0f + 0.001f));
    runner.CloseWorld(world);

    CHECK(counts[SystemContextPhase::Start] == 1);
    CHECK(counts[SystemContextPhase::Sim] == Steps);
    CHECK(counts[SystemContextPhase::View] == 1);
    CHECK(counts[SystemContextPhase::Stop] == 1);
    CHECK(counts[SystemContextPhase::Replay] == 0);
    CHECK(lastSteps == 1);
    CHECK(std::ranges::all_of(named, [world](const WorldInstanceId id) { return id == world; }));
}

TEST_CASE("A replayed step runs under the world holding its scene, at that world's tick rate")
{
    WorldProbe::Reset();

    TypeRegistry types;
    SystemRegistry systems;
    systems.Register<WorldProbe>();
    TestSupport::TestServices services;
    WorldRunner runner(WorldRunnerInfo{
        .Types = &types, .Systems = &systems, .Presentation = &services.GetPresentationScopes()});
    runner.SetContextFactory(services.Factory());

    WorldOpenInfo first = WorldOf({SystemIdOf<WorldProbe>()});
    first.SimTickRate = 60;
    WorldOpenInfo second = WorldOf({SystemIdOf<WorldProbe>()});
    second.SimTickRate = 20;
    const WorldInstanceId a = runner.OpenWorld(first);
    const WorldInstanceId b = runner.OpenWorld(second);
    Scene& sceneB = runner.ResolveWorld(b)->GetScene();
    REQUIRE(runner.FindWorld(sceneB) == b);
    REQUIRE(runner.FindWorld(runner.ResolveWorld(a)->GetScene()) == a);

    runner.ReplaySimStep(sceneB, 7);

    CHECK(WorldProbe::Stepped[&sceneB] == b);
    CHECK(WorldProbe::StepDelta[&sceneB] == doctest::Approx(1.0f / 20.0f));
    CHECK(WorldProbe::StepReplay[&sceneB]);
    CHECK(WorldProbe::Stepped.size() == 1);
}

TEST_CASE("A test services context reaches working audio and localization")
{
    TestSupport::TestServices services;
    TypeRegistry types;
    const Unique<Scene> scene = Scene::Create(types);
    const SystemContext context = services.Make(SystemContextRequest{.Scene = *scene});

    // A resident one-frame clip: the null device plays it as any device would.
    const CookedAudioHeader header{
        .Version = CookedAudioVersion,
        .Storage = static_cast<u32>(CookedAudioStorage::Pcm),
        .SampleFormat = static_cast<u32>(CookedAudioSampleFormat::F32),
        .Codec = static_cast<u32>(CookedAudioCodec::None),
        .SampleRate = 48000,
        .Channels = 1,
        .FrameCount = 64,
    };
    std::vector<u8> blob(sizeof(header) + 64 * sizeof(f32), 0);
    std::memcpy(blob.data(), &header, sizeof(header));
    const Result<Ref<Audio::AudioClip>> clip = Audio::AudioClip::Decode(blob);
    REQUIRE(clip.has_value());

    CHECK(context.Audio.PlayOneShot(AssetManager::Adopt<Audio::AudioClip>(*clip)).IsValid());
    CHECK(context.Localization.Get("menu.title") == "menu.title");
}
