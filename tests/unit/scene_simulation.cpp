// SystemRegistry + SceneSimulation: registration order, lifecycle call counts, and
// IsEmpty/Count. Pure CPU — no Context, no Vulkan symbol touched.
//
// SystemContext aggregates an AssetManager& and a const Input&, both of which are
// device-bound (an AssetManager needs a Renderer::Context, an Input needs a Window),
// so neither can be constructed in a pure unit test. The driver only forwards the
// context to each system and the counting systems here never touch it, so a context
// over never-dereferenced storage keeps the test device-free while still exercising
// the real SceneSimulation::Start/Update/Stop path.

#include <doctest/doctest.h>

#include <vector>

#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneSimulation.h>
#include <Veng/Scene/SceneSystem.h>
#include <Veng/Scene/SystemRegistry.h>

using namespace Veng;

namespace
{
    // Shared call log so the two registered system types record into one ordering,
    // proving registration order is the run order.
    std::vector<int> g_UpdateOrder;

    template <int Tag>
    struct CountingSystem final : SceneSystem
    {
        // Per-type counters; reset between cases by reconstructing the registry/sim.
        static inline int Starts = 0;
        static inline int Updates = 0;
        static inline int Stops = 0;

        static void Reset()
        {
            Starts = 0;
            Updates = 0;
            Stops = 0;
        }

        void OnStart(Scene&, const SystemContext&) override { ++Starts; }
        void OnUpdate(Scene&, f32, const SystemContext&) override
        {
            ++Updates;
            g_UpdateOrder.push_back(Tag);
        }
        void OnStop(Scene&, const SystemContext&) override { ++Stops; }
    };

    using SystemA = CountingSystem<1>;
    using SystemB = CountingSystem<2>;

    // A View-phase counterpart to CountingSystem: same call log, but it overrides
    // GetPhase() to View so the driver runs it after every Sim system.
    template <int Tag>
    struct ViewCountingSystem final : SceneSystem
    {
        Phase GetPhase() const override { return Phase::View; }
        void OnUpdate(Scene&, f32, const SystemContext&) override { g_UpdateOrder.push_back(Tag); }
    };
}

// The catalog reads each registered system's identity off its VengSystem trait. The
// counting systems are templates, so a partial specialisation derives a distinct
// SystemId per Tag from a fixed base; the names embed the tag for readability.
namespace Veng
{
    template <int Tag>
    struct VengSystem<CountingSystem<Tag>>
    {
        static constexpr SystemId Id = 0x51110000000000F0ULL + static_cast<SystemId>(Tag);
        static string Name() { return "CountingSystem"; }
    };

    template <int Tag>
    struct VengSystem<ViewCountingSystem<Tag>>
    {
        static constexpr SystemId Id = 0x51220000000000F0ULL + static_cast<SystemId>(Tag);
        static string Name() { return "ViewCountingSystem"; }
    };
}

namespace
{

    TypeRegistry MakeRegistry()
    {
        return TypeRegistry{};
    }

    // A SystemContext the driver forwards but no system here dereferences. The
    // storage is never read as an AssetManager/Input; it only provides bindable
    // lvalues for the aggregate's references.
    struct ContextStorage
    {
        alignas(16) unsigned char AssetsBytes[64]{};
        alignas(16) unsigned char InputBytes[64]{};

        SystemContext Make()
        {
            return SystemContext{
                .Assets = *reinterpret_cast<AssetManager*>(AssetsBytes),
                .Input = *reinterpret_cast<Input*>(InputBytes),
            };
        }
    };
}

TEST_CASE("SystemRegistry reports Count and Instantiate builds one of each")
{
    SystemRegistry registry;
    CHECK(registry.Count() == 0);

    registry.Register<SystemA>();
    registry.Register<SystemB>();
    CHECK(registry.Count() == 2);

    const vector<Unique<SceneSystem>> systems = registry.Instantiate();
    REQUIRE(systems.size() == 2);
    CHECK(systems[0] != nullptr);
    CHECK(systems[1] != nullptr);
}

TEST_CASE("SceneSimulation IsEmpty tracks whether any systems were registered")
{
    const SystemRegistry empty;
    const SceneSimulation emptySim(empty);
    CHECK(emptySim.IsEmpty());

    SystemRegistry one;
    one.Register<SystemA>();
    const SceneSimulation oneSim(one);
    CHECK_FALSE(oneSim.IsEmpty());
}

TEST_CASE("SceneSimulation drives Start/Update/Stop on each system in registration order")
{
    SystemA::Reset();
    SystemB::Reset();
    g_UpdateOrder.clear();

    TypeRegistry types = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(types);

    SystemRegistry registry;
    registry.Register<SystemA>();
    registry.Register<SystemB>();

    SceneSimulation sim(registry);

    ContextStorage storage;

    sim.Start(*scene, storage.Make());
    CHECK(SystemA::Starts == 1);
    CHECK(SystemB::Starts == 1);

    constexpr int UpdateCount = 3;
    for (int i = 0; i < UpdateCount; ++i)
    {
        sim.Update(*scene, 0.016f, storage.Make());
    }
    CHECK(SystemA::Updates == UpdateCount);
    CHECK(SystemB::Updates == UpdateCount);

    sim.Stop(*scene, storage.Make());
    CHECK(SystemA::Stops == 1);
    CHECK(SystemB::Stops == 1);

    // Each Update visits the systems in registration order: A (1) then B (2).
    REQUIRE(g_UpdateOrder.size() == static_cast<usize>(UpdateCount) * 2);
    for (int i = 0; i < UpdateCount; ++i)
    {
        CHECK(g_UpdateOrder[i * 2] == 1);
        CHECK(g_UpdateOrder[i * 2 + 1] == 2);
    }
}

TEST_CASE("Update runs all Sim systems before all View systems, registration order within each")
{
    g_UpdateOrder.clear();

    TypeRegistry types = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(types);

    // Interleave registration to prove the partition is by phase, not by position: a View
    // system registered before a Sim system still runs after it. Sim tags 1/2, View tags 3/4.
    SystemRegistry registry;
    registry.Register<ViewCountingSystem<3>>();
    registry.Register<SystemA>();
    registry.Register<ViewCountingSystem<4>>();
    registry.Register<SystemB>();

    SceneSimulation sim(registry);
    ContextStorage storage;
    sim.Update(*scene, 0.016f, storage.Make());

    // Sim phase first in registration order (1, 2), then View phase in registration order (3, 4).
    REQUIRE(g_UpdateOrder.size() == 4);
    CHECK(g_UpdateOrder[0] == 1);
    CHECK(g_UpdateOrder[1] == 2);
    CHECK(g_UpdateOrder[2] == 3);
    CHECK(g_UpdateOrder[3] == 4);
}

TEST_CASE("A Sim-default system ticks unchanged when a View system is present")
{
    SystemA::Reset();
    g_UpdateOrder.clear();

    TypeRegistry types = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(types);

    SystemRegistry registry;
    registry.Register<SystemA>();
    registry.Register<ViewCountingSystem<3>>();

    SceneSimulation sim(registry);
    ContextStorage storage;

    constexpr int UpdateCount = 2;
    for (int i = 0; i < UpdateCount; ++i)
    {
        sim.Update(*scene, 0.016f, storage.Make());
    }

    // The Sim-default SystemA ticks once per Update exactly as before the phase split.
    CHECK(SystemA::Updates == UpdateCount);
}

TEST_CASE("SystemRegistry catalog enumerates entries and resolves each id back to a system")
{
    SystemRegistry registry;
    registry.Register<SystemA>();
    registry.Register<ViewCountingSystem<3>>();

    // Entries enumerate the catalog in registration order, carrying each system's id +
    // name without instantiating anything.
    const vector<SystemEntry>& entries = registry.Entries();
    REQUIRE(entries.size() == 2);
    CHECK(entries[0].Id == SystemIdOf<SystemA>());
    CHECK(entries[0].Name == "CountingSystem");
    CHECK(entries[1].Id == SystemIdOf<ViewCountingSystem<3>>());
    CHECK(entries[1].Name == "ViewCountingSystem");

    // Each registered id resolves to a freshly built system.
    CHECK(registry.Instantiate(SystemIdOf<SystemA>()) != nullptr);
    CHECK(registry.Instantiate(SystemIdOf<ViewCountingSystem<3>>()) != nullptr);

    // An unknown id resolves to nothing.
    CHECK(registry.Instantiate(0xDEADBEEFDEADBEEFULL) == nullptr);
}

TEST_CASE("SceneSimulation from an ordered id set runs exactly those systems, in that order")
{
    g_UpdateOrder.clear();

    TypeRegistry types = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(types);

    // Register four systems; the simulation selects a subset, ordered opposite to
    // registration, proving the id list — not registration order — drives the run order.
    SystemRegistry registry;
    registry.Register<SystemA>();               // Sim, tag 1
    registry.Register<SystemB>();               // Sim, tag 2
    registry.Register<ViewCountingSystem<3>>(); // View, tag 3
    registry.Register<ViewCountingSystem<4>>(); // View, tag 4

    const vector<SystemId> active = {
        SystemIdOf<ViewCountingSystem<4>>(),
        SystemIdOf<SystemB>(),
        SystemIdOf<ViewCountingSystem<3>>(),
    };
    SceneSimulation sim(registry, active);
    ContextStorage storage;
    sim.Update(*scene, 0.016f, storage.Make());

    // SystemA (tag 1) was not named, so it never runs. The Sim phase runs first (tag 2),
    // then the View phase in the named order (4 before 3).
    REQUIRE(g_UpdateOrder.size() == 3);
    CHECK(g_UpdateOrder[0] == 2);
    CHECK(g_UpdateOrder[1] == 4);
    CHECK(g_UpdateOrder[2] == 3);
}

TEST_CASE("SceneSimulation from an id set skips an id absent from the catalog")
{
    g_UpdateOrder.clear();

    TypeRegistry types = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(types);

    SystemRegistry registry;
    registry.Register<SystemA>();

    const vector<SystemId> active = {
        0xCAFEBABECAFEBABEULL, // not in the catalog — skipped
        SystemIdOf<SystemA>(),
    };
    const SceneSimulation sim(registry, active);
    CHECK_FALSE(sim.IsEmpty());

    SceneSimulation runnable(registry, active);
    ContextStorage storage;
    runnable.Update(*scene, 0.016f, storage.Make());

    // Only the present id ran.
    REQUIRE(g_UpdateOrder.size() == 1);
    CHECK(g_UpdateOrder[0] == 1);
}

TEST_CASE("The all-registered convenience builds every registered system")
{
    SystemRegistry registry;
    registry.Register<SystemA>();
    registry.Register<SystemB>();
    registry.Register<ViewCountingSystem<3>>();

    const SceneSimulation sim(registry);
    CHECK_FALSE(sim.IsEmpty());
    CHECK(registry.Count() == 3);
}
