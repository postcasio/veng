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
