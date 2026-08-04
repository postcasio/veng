// The bindless arrays across a world's lifetime — the budget a consumer that opens and closes
// worlds runs against.
//
// A SceneSystem is constructed per open world, so a system that registers a bindless resource
// registers one *per world*: what a single-world reading calls the cost is multiplied by however
// many worlds are open, and what a single-world reading cannot see at all is whether the cost comes
// back when the world is dropped. Both are fatal when they go wrong — every arrayed binding has a
// fixed capacity and exhausting one is an assert, reached on a registration that is in itself
// perfectly ordinary — and neither is visible from a case that opens one world and looks at it. So
// the cases here open several, and open them repeatedly:
//
//  - a world opened, driven and closed returns every slot its systems took, so a run that switches
//    worlds holds a steady count rather than walking an array down to its exhaustion. The reading is
//    per cycle, not cumulative: a cycle handing back one slot fewer than it took is the shape that
//    exhausts an array after as many cycles as it holds slots;
//  - several worlds open at once each hold their own registrations, and the occupancy is their sum.
//    This is the multiplication stated as a measurement rather than assumed: a consumer sizing a
//    per-world pool reads its own per-world cost here and multiplies by the worlds it holds open.
//
// The baseline is taken after one warm-up cycle rather than before it. The first world in a process
// loads the shared assets its systems ask for — a parent material, its pipeline, its textures — and
// those stay resident in the asset manager for the rest of the run, so a reading bracketing the
// first cycle measures that one-off residency and calls it a leak.
//
// It needs a Context for the Application and for the registry itself, so it rides the gpu band
// though it pins no pixels. Cooker-gated: the per-world system builds instances over a cooked
// parent material, which is the shape a consumer's own per-world pool takes.

#ifdef GPU_GBUFFER_FIXTURE_DIR

#include <doctest/doctest.h>

#include <algorithm>
#include <vector>

#include <Veng/Application.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Level.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Prefab.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Input.h>
#include <Veng/Path.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/BindlessRegistry.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneSystem.h>
#include <Veng/Scene/SystemRegistry.h>
#include <Veng/Task/TaskSystem.h>
#include <Veng/WorldRunner.h>

#include "support/TempPath.h"

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // The brick Surface parent in the g-buffer fixture pack — the parent the per-world pool
    // instances. Any parent would do; this one is already cooked by the band.
    constexpr AssetId BrickParentId{0x232BULL};

    // How many instances one world's pool holds. Small, and deliberately more than one: a per-world
    // cost of exactly one slot cannot tell a released slot from a slot the next world reused.
    constexpr usize InstancesPerWorld = 4;

    // The parent every PoolSystem instances, published by the app before it opens a world. A system
    // is constructed by the world's own registry with no arguments, so the handle it needs reaches
    // it here rather than through a constructor.
    AssetHandle<Material> g_Parent;

    // A per-world material pool, the shape a consumer's own per-world presentation system takes: it
    // builds its instances on its first drive and holds them for the world's lifetime, so dropping
    // the world is what returns their slots.
    //
    // Each pool self-registers in a process-wide list, because a world's systems are reachable only
    // from inside their own drive: SceneSimulation exposes no lookup by type, and a case here needs
    // to ask "has every open world's pool landed" from the app's own step. The list is also the
    // proof the systems are destroyed at all — a world closed with its pool still listed would show
    // up as an over-count on the next reading.
    struct PoolSystem final : SceneSystem
    {
        static inline vector<PoolSystem*> Live;

        PoolSystem() { Live.push_back(this); }
        ~PoolSystem() override { std::erase(Live, this); }

        PoolSystem(const PoolSystem&) = delete;
        PoolSystem& operator=(const PoolSystem&) = delete;

        [[nodiscard]] Phase GetPhase() const override { return Phase::View; }

        void OnUpdate(Scene&, f32, const SystemContext& context) override
        {
            if (!m_Instances.empty())
            {
                return;
            }
            for (usize i = 0; i < InstancesPerWorld; ++i)
            {
                m_Instances.push_back(context.Assets.Build<MaterialInstance>(MaterialInstanceInfo{
                    .Name = "Pool Instance",
                    .Context = &context.Assets.GetContext(),
                    .Parent = g_Parent,
                }));
            }
        }

        /// @brief Whether every instance this world's pool asked for has landed.
        [[nodiscard]] bool IsResident() const
        {
            return m_Instances.size() == InstancesPerWorld &&
                   std::ranges::all_of(m_Instances,
                                       [](const AssetHandle<MaterialInstance>& instance)
                                       { return instance.IsLoaded(); });
        }

    private:
        vector<AssetHandle<MaterialInstance>> m_Instances;
    };

    // Whether @p count pools exist and every one of them has landed its whole allocation.
    bool PoolsResident(const usize count)
    {
        return PoolSystem::Live.size() == count &&
               std::ranges::all_of(PoolSystem::Live,
                                   [](const PoolSystem* pool) { return pool->IsResident(); });
    }
}

namespace Veng
{
    template <>
    struct VengSystem<PoolSystem>
    {
        static constexpr SystemId Id = 0xB0D6E7000000000AULL;
        static string Name() { return "PoolSystem"; }
    };
}

namespace
{
    path CookBrickPack()
    {
        const path fixtureDir = path(GPU_GBUFFER_FIXTURE_DIR);
        const path outArchive = Veng::TestSupport::TempDir() / "veng_world_slot_budget.vengpack";

        Cook::Cooker cooker;
        Cook::RegisterBuiltinImporters(cooker);
        const VoidResult cookResult =
            cooker.CookPack(fixtureDir / "gbuffer_pack.json", outArchive, {}, nullptr, nullptr,
                            nullptr, nullptr, {}, path(VENG_CORE_SHADER_DIR));
        REQUIRE_MESSAGE(cookResult.has_value(), cookResult.error());
        return outArchive;
    }

    // A headless application driven a frame at a time by a closure, so a case writes its
    // open/drive/close sequence as a state machine over the frame index and reads the registry from
    // inside the running engine — the only place the counts mean anything.
    class BudgetApp final : public Application
    {
    public:
        using Application::Application;

        // Runs before ~Application, while the engine state is alive, so the published parent drops
        // its cache entry — and with it the pipeline and buffers behind it — before the context that
        // owns them is torn down. A file-static handle outliving the context is a resource still
        // allocated when the allocator is destroyed, which aborts.
        ~BudgetApp() override { g_Parent = {}; }

        function<void(BudgetApp&, i32)> StepFn;
        i32 Frames = 1;
        i32 Current = 0;

        /// @brief Opens a runner-owned world running the pool system, and returns its handle.
        WorldInstanceId AddWorld()
        {
            const AssetHandle<Prefab> prefab =
                GetAssetManager().Adopt<Prefab>(Prefab::Create({}, {}));
            const AssetHandle<Level> level = GetAssetManager().Adopt<Level>(
                Level::Create(prefab, vector<SystemId>{SystemIdOf<PoolSystem>()}, GameModeConfig{},
                              LevelRenderSettings{}));
            m_Levels.push_back(level);
            return GetWorldRunner().OpenWorld(WorldOpenInfo{
                .Source = level,
                .MakeStartContext =
                    [this]
                {
                    return SystemContext{.Assets = GetAssetManager(),
                                         .Input = GetInput(),
                                         .Tasks = GetTaskSystem(),
                                         .Audio = GetAudioEngine()};
                },
            });
        }

        /// @brief Ends the run — the step's own exit, once its readings are taken.
        void Stop() { RequestExit(); }

        /// @brief The registry's free counts as of this frame.
        [[nodiscard]] BindlessCapacity FreeSlots()
        {
            return GetRenderContext().GetBindlessRegistry().GetFreeSlots();
        }

    protected:
        void OnInitialize() override
        {
            REQUIRE(GetAssetManager().Mount(CookBrickPack()).has_value());
            const AssetResult<AssetHandle<Material>> parent =
                GetAssetManager().LoadSync<Material>(BrickParentId);
            REQUIRE(parent.has_value());
            g_Parent = *parent;
        }

        void OnUpdate(f32) override
        {
            if (StepFn)
            {
                StepFn(*this, Current);
            }
            if (++Current >= Frames)
            {
                RequestExit();
            }
        }

    private:
        vector<AssetHandle<Level>> m_Levels;
    };

    ApplicationInfo HeadlessInfo()
    {
        ApplicationInfo info;
        info.Name = "veng-world-slot-budget-test";
        info.Headless = true;
        info.ImGui = std::nullopt;
        return info;
    }
}

TEST_CASE("world slot budget: a world opened and closed returns every bindless slot it took")
{
    // One cycle is: open a world, drive it until its pool has landed, close it, and let the
    // deferred releases settle. The settle is not optional — a slot released while frame-in-flight
    // i is current returns to the free list only when AcquireNextFrame makes i current again, so a
    // reading taken too early reports the deferral rather than the ownership.
    constexpr i32 SettleFrames = 8;
    constexpr i32 Cycles = 6;

    struct Reading
    {
        BindlessCapacity Capacity;
        bool Taken = false;
    };
    Reading baseline;
    vector<BindlessCapacity> perCycle;

    WorldInstanceId open;
    i32 cycle = 0;
    i32 settling = 0;

    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;
    systems.Register<PoolSystem>();

    BudgetApp app(HeadlessInfo(), types, systems);
    app.Frames = 4000; // a ceiling, not a schedule: the step exits as soon as the cycles are done
    app.StepFn = [&](BudgetApp& a, i32)
    {
        if (settling > 0)
        {
            if (--settling > 0)
            {
                return;
            }
            // The cycle is complete: the world is gone and its releases have cycled through.
            if (!baseline.Taken)
            {
                baseline = Reading{.Capacity = a.FreeSlots(), .Taken = true};
            }
            else
            {
                perCycle.push_back(a.FreeSlots());
            }
            if (++cycle > Cycles)
            {
                a.Stop();
                return;
            }
        }

        if (!open.IsValid())
        {
            open = a.AddWorld();
            return;
        }
        if (!PoolsResident(1))
        {
            return;
        }
        a.GetWorldRunner().CloseWorld(open);
        open = WorldInstanceId{};
        settling = SettleFrames;
    };

    REQUIRE(app.Run({}) == 0);

    REQUIRE(baseline.Taken);
    REQUIRE(baseline.Capacity.Materials > 0);
    REQUIRE(perCycle.size() == static_cast<usize>(Cycles));

    // The count is the invariant, not any particular figure. A cycle that hands back one slot fewer
    // than it took reads here as a monotonically falling count — which is the exhaustion, seen early
    // enough to name the cycle it costs rather than the registration that happens to be last.
    for (const BindlessCapacity& after : perCycle)
    {
        CHECK(after.Materials == baseline.Capacity.Materials);
        CHECK(after.Textures == baseline.Capacity.Textures);
        CHECK(after.Samplers == baseline.Capacity.Samplers);
        CHECK(after.StorageImages == baseline.Capacity.StorageImages);
        CHECK(after.StorageBuffers == baseline.Capacity.StorageBuffers);
    }
}

TEST_CASE("world slot budget: worlds open at once each hold their own slots, and the peak is their "
          "sum")
{
    // Enough worlds that the per-world cost is read as a slope rather than as one difference, and
    // few enough that a system whose per-world cost is a pool rather than a handful still fits.
    constexpr usize Worlds = 5;
    constexpr i32 SettleFrames = 8;

    BindlessCapacity before{};
    BindlessCapacity peak{};
    BindlessCapacity after{};
    bool measured = false;

    vector<WorldInstanceId> worlds;
    i32 stage = 0;
    i32 settling = 0;

    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;
    systems.Register<PoolSystem>();

    BudgetApp app(HeadlessInfo(), types, systems);
    app.Frames = 4000;
    app.StepFn = [&](BudgetApp& a, i32)
    {
        if (settling > 0 && --settling > 0)
        {
            return;
        }
        switch (stage)
        {
        case 0:
            // A warm-up world, so the shared assets a pool's parent pulls in are already resident
            // and the reading below brackets the per-world cost alone.
            worlds.push_back(a.AddWorld());
            stage = 1;
            return;
        case 1:
            if (!PoolsResident(worlds.size()))
            {
                return;
            }
            a.GetWorldRunner().CloseWorld(worlds.front());
            worlds.clear();
            settling = SettleFrames;
            stage = 2;
            return;
        case 2:
            before = a.FreeSlots();
            for (usize i = 0; i < Worlds; ++i)
            {
                worlds.push_back(a.AddWorld());
            }
            stage = 3;
            return;
        case 3:
            if (!PoolsResident(worlds.size()))
            {
                return;
            }
            peak = a.FreeSlots();
            for (const WorldInstanceId world : worlds)
            {
                a.GetWorldRunner().CloseWorld(world);
            }
            worlds.clear();
            settling = SettleFrames;
            stage = 4;
            return;
        default:
            after = a.FreeSlots();
            measured = true;
            a.Stop();
            return;
        }
    };

    REQUIRE(app.Run({}) == 0);
    REQUIRE(measured);

    // Each open world holds its own pool: the occupancy is per-world cost times worlds, not one
    // pool shared between them. This is the arithmetic a consumer sizes against — a per-world pool
    // that fits alone can still exhaust the table once the worlds that hold one are counted.
    REQUIRE(before.Materials >= peak.Materials);
    const u32 held = before.Materials - peak.Materials;
    CHECK(held == static_cast<u32>(Worlds * InstancesPerWorld));

    // And the whole of it comes back, so the peak is a peak rather than a step.
    CHECK(after.Materials == before.Materials);
    CHECK(after.Textures == before.Textures);
    CHECK(after.Samplers == before.Samplers);
    CHECK(after.StorageImages == before.StorageImages);
    CHECK(after.StorageBuffers == before.StorageBuffers);
}

#endif
