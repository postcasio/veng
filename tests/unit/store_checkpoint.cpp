// StoreCheckpoint: the timed and on-demand whole-store checkpoint over a runner's live worlds. The
// properties pinned: an on-demand checkpoint captures every live world (not just one) and its flush
// is durable across a cold reopen; the timed cadence fires only once the interval accrues; and a
// null-resolving store source is the no-op posture rather than an error.

#include <doctest/doctest.h>

#include <Veng/Persistence/Store.h>
#include <Veng/Persistence/StoreCheckpoint.h>
#include <Veng/Persistence/StorePatterns.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SystemRegistry.h>
#include <Veng/World.h>
#include <Veng/WorldRunner.h>

#include <support/TempPath.h>

#include <atomic>
#include <filesystem>

using namespace Veng;

namespace
{
    // A fresh, unique slot directory per case, removed on destruction.
    struct TempSlot
    {
        path Dir;

        TempSlot()
        {
            static std::atomic<u64> counter{0};
            Dir =
                TestSupport::TempDir() /
                fmt::format("store-checkpoint-{}", counter.fetch_add(1, std::memory_order_relaxed));
            std::filesystem::remove_all(Dir);
        }

        ~TempSlot() { std::filesystem::remove_all(Dir); }
    };

    constexpr StoreFamilyId CheckpointFamily{0x7E57020000000001ULL};

    struct CheckpointTag
    {
        u64 Slot = 0;
    };

    struct CheckpointPayload
    {
        u32 Value = 0;
    };
}

VE_REFLECT(::CheckpointTag, 0x7E57020000000010ULL)
VE_FIELD(Slot)
VE_REFLECT_END();

VE_REFLECT(::CheckpointPayload, 0x7E57020000000011ULL)
VE_FIELD(Value)
VE_REFLECT_END();

namespace
{
    optional<StoreKey> KeyOfTag(const CheckpointTag& tag)
    {
        return StoreKey{.Lo = tag.Slot, .Hi = 0};
    }

    TypeRegistry MakeRegistry()
    {
        TypeRegistry types;
        types.Register<CheckpointTag>();
        types.Register<CheckpointPayload>();
        return types;
    }

    // Opens a world holding one marked, keyed entity, so a capture writes one record per world.
    WorldInstanceId OpenMarkedWorld(WorldRunner& runner, const u64 slot, const u32 value)
    {
        const WorldInstanceId world = runner.OpenWorld(WorldOpenInfo{
            .SimTickRate = 60,
            .StartSimulation = false,
        });
        Scene& scene = runner.ResolveWorld(world)->GetScene();
        const Entity entity = scene.CreateEntity();
        scene.Add<CheckpointTag>(entity, CheckpointTag{.Slot = slot});
        scene.Add<CheckpointPayload>(entity, CheckpointPayload{.Value = value});
        return world;
    }
}

TEST_CASE("StoreCheckpoint captures every live world and the flush survives a cold reopen")
{
    const TempSlot slot;
    TypeRegistry types = MakeRegistry();
    SystemRegistry systems;
    WorldRunner runner(WorldRunnerInfo{.Types = &types, .Systems = &systems});
    OpenMarkedWorld(runner, 1, 11);
    OpenMarkedWorld(runner, 2, 22);

    {
        Result<Unique<Store>> store = Store::Open(slot.Dir);
        REQUIRE(store.has_value());
        (*store)->RegisterFamily(ComponentSetFamily<CheckpointTag, CheckpointPayload>(
            CheckpointFamily, "checkpoints", KeyOfTag, types));

        StoreCheckpoint checkpoint(StoreCheckpoint::Info{
            .Runner = &runner, .StoreSource = [&store] { return store->get(); }});
        checkpoint.CheckpointNow();

        // Both worlds' records are captured — the checkpoint walks the whole runner, not the
        // presented world — and the measured costs hold values in every build configuration.
        CHECK((*store)->Read(CheckpointFamily, StoreKey{.Lo = 1, .Hi = 0}).has_value());
        CHECK((*store)->Read(CheckpointFamily, StoreKey{.Lo = 2, .Hi = 0}).has_value());
        const auto [captureMs, flushMs] = checkpoint.LastCostMs();
        CHECK(captureMs >= 0.0);
        CHECK(flushMs >= 0.0);
    }

    // The flush was durable: a cold reopen reads both records off disk.
    Result<Unique<Store>> reopened = Store::Open(slot.Dir);
    REQUIRE(reopened.has_value());
    (*reopened)->RegisterFamily(ComponentSetFamily<CheckpointTag, CheckpointPayload>(
        CheckpointFamily, "checkpoints", KeyOfTag, types));
    CHECK((*reopened)->Read(CheckpointFamily, StoreKey{.Lo = 1, .Hi = 0}).has_value());
    CHECK((*reopened)->Read(CheckpointFamily, StoreKey{.Lo = 2, .Hi = 0}).has_value());
}

TEST_CASE("StoreCheckpoint's cadence fires only once the interval accrues, and no store is a no-op")
{
    const TempSlot slot;
    TypeRegistry types = MakeRegistry();
    SystemRegistry systems;
    WorldRunner runner(WorldRunnerInfo{.Types = &types, .Systems = &systems});
    OpenMarkedWorld(runner, 7, 77);

    Result<Unique<Store>> store = Store::Open(slot.Dir);
    REQUIRE(store.has_value());
    (*store)->RegisterFamily(ComponentSetFamily<CheckpointTag, CheckpointPayload>(
        CheckpointFamily, "checkpoints", KeyOfTag, types));

    StoreCheckpoint checkpoint(
        StoreCheckpoint::Info{.Runner = &runner,
                              .StoreSource = [&store] { return store->get(); },
                              .IntervalSeconds = 10.0});

    // Below the interval nothing captures; crossing it captures once.
    checkpoint.Update(9.0f);
    CHECK(!(*store)->Read(CheckpointFamily, StoreKey{.Lo = 7, .Hi = 0}).has_value());
    checkpoint.Update(1.5f);
    CHECK((*store)->Read(CheckpointFamily, StoreKey{.Lo = 7, .Hi = 0}).has_value());

    // A null-resolving source is the no-op posture: neither the cadence nor the on-demand path
    // touches anything, and neither faults.
    StoreCheckpoint storeless(StoreCheckpoint::Info{
        .Runner = &runner, .StoreSource = [] { return nullptr; }, .IntervalSeconds = 0.5});
    storeless.Update(1.0f);
    storeless.CheckpointNow();
    CHECK(storeless.LastCostMs() == std::pair{0.0, 0.0});
}
