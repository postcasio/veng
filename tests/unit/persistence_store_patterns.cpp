// Store-pattern tests: the edge semantics ComponentSetFamily and the singleton accessors pin, one
// case per divergence point two hand-written registrars could reasonably differ at — the nullopt
// key skip, the zero-component entity writing no record, rehydrate adding an absent component, an
// unmatched blob logged rather than swallowed, first claimant wins, and the singleton's blob-level
// read-modify-write.

#include <doctest/doctest.h>

#include <Veng/Log.h>
#include <Veng/Persistence/StorePatterns.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Scene.h>

#include <support/TempPath.h>

#include <atomic>
#include <filesystem>

using namespace Veng;

namespace
{
    // A fresh, unique slot directory per case under the process's scratch tree, removed on
    // destruction.
    struct TempSlot
    {
        path Dir;

        TempSlot()
        {
            static std::atomic<u64> counter{0};
            Dir = TestSupport::TempDir() /
                  fmt::format("store-patterns-{}", counter.fetch_add(1, std::memory_order_relaxed));
            std::filesystem::remove_all(Dir);
        }

        ~TempSlot() { std::filesystem::remove_all(Dir); }
    };

    // Collects the warnings a case provokes, so the log-once semantics are assertable.
    struct WarnCapture
    {
        vector<string> Warnings;

        WarnCapture()
        {
            Log::SetSink(
                [this](const Log::Level level, const string_view message)
                {
                    if (level == Log::Level::Warn)
                    {
                        Warnings.emplace_back(message);
                    }
                });
        }

        ~WarnCapture() { Log::SetSink(nullptr); }
    };

    constexpr StoreFamilyId PatternFamily{0x7E57010000000001ULL};
    constexpr StoreFamilyId SingletonTestFamily{0x7E57010000000002ULL};
}

// The test fixtures: a marker whose Slot names the record key (zero meaning "no key"), two
// captured components, and a third reflected type the family captures nothing for.
namespace
{
    struct PatternTag
    {
        u64 Slot = 0;
    };

    struct PatternAlpha
    {
        u32 Value = 0;
    };

    struct PatternBeta
    {
        u32 Weight = 0;
    };

    struct PatternStray
    {
        u32 Noise = 0;
    };
}

VE_REFLECT(::PatternTag, 0x7E57010000000010ULL)
VE_FIELD(Slot)
VE_REFLECT_END();

VE_REFLECT(::PatternAlpha, 0x7E57010000000011ULL)
VE_FIELD(Value)
VE_REFLECT_END();

VE_REFLECT(::PatternBeta, 0x7E57010000000012ULL)
VE_FIELD(Weight)
VE_REFLECT_END();

VE_REFLECT(::PatternStray, 0x7E57010000000013ULL)
VE_FIELD(Noise)
VE_REFLECT_END();

namespace
{
    // A slot's Lo half is the record key; slot 0 resolves to no key at all.
    optional<StoreKey> KeyOfTag(const PatternTag& tag)
    {
        if (tag.Slot == 0)
        {
            return std::nullopt;
        }
        return StoreKey{.Lo = tag.Slot, .Hi = 0};
    }

    StoreFamily MakePatternFamily(const TypeRegistry& types)
    {
        return ComponentSetFamily<PatternTag, PatternAlpha, PatternBeta>(PatternFamily, "patterns",
                                                                         KeyOfTag, types);
    }

    // The fixtures' registry: the pooled marker plus the three reflected components.
    TypeRegistry MakeRegistry()
    {
        TypeRegistry types;
        types.Register<PatternTag>();
        types.Register<PatternAlpha>();
        types.Register<PatternBeta>();
        types.Register<PatternStray>();
        return types;
    }

    Entity SpawnTagged(Scene& scene, const u64 slot)
    {
        const Entity entity = scene.CreateEntity();
        scene.Add<PatternTag>(entity, PatternTag{.Slot = slot});
        return entity;
    }
}

TEST_CASE("component-set family round-trips captured components across flush and reopen")
{
    const TempSlot slot;
    TypeRegistry types = MakeRegistry();
    {
        const Unique<Scene> scene = Scene::Create(types);
        const Entity entity = SpawnTagged(*scene, 7);
        scene->Add<PatternAlpha>(entity, PatternAlpha{.Value = 42});
        scene->Add<PatternBeta>(entity, PatternBeta{.Weight = 9});

        Result<Unique<Store>> store = Store::Open(slot.Dir);
        REQUIRE(store);
        (*store)->RegisterFamily(MakePatternFamily(types));
        (*store)->CaptureScene(*scene);
        REQUIRE((*store)->Flush());
    }

    Result<Unique<Store>> reopened = Store::Open(slot.Dir);
    REQUIRE(reopened);
    (*reopened)->RegisterFamily(MakePatternFamily(types));

    const Unique<Scene> fresh = Scene::Create(types);
    const Entity restored = SpawnTagged(*fresh, 7);
    (*reopened)->RehydrateScene(*fresh);

    const PatternAlpha* const alpha = fresh->TryGet<PatternAlpha>(restored);
    const PatternBeta* const beta = fresh->TryGet<PatternBeta>(restored);
    REQUIRE(alpha != nullptr);
    REQUIRE(beta != nullptr);
    CHECK(alpha->Value == 42);
    CHECK(beta->Weight == 9);
}

TEST_CASE("an entity whose key resolves to nullopt is skipped")
{
    const TempSlot slot;
    TypeRegistry types = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(types);
    const Entity entity = SpawnTagged(*scene, 0);
    scene->Add<PatternAlpha>(entity, PatternAlpha{.Value = 5});

    Result<Unique<Store>> store = Store::Open(slot.Dir);
    REQUIRE(store);
    (*store)->RegisterFamily(MakePatternFamily(types));
    (*store)->CaptureScene(*scene);

    // Never written under a sentinel key either: the zero key holds nothing.
    CHECK((*store)->GetRecordCount() == 0);
    CHECK_FALSE((*store)->Read(PatternFamily, StoreKey{.Lo = 0, .Hi = 0}).has_value());
}

TEST_CASE("an entity capturing zero components contributes no record")
{
    const TempSlot slot;
    TypeRegistry types = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(types);
    SpawnTagged(*scene, 3);

    Result<Unique<Store>> store = Store::Open(slot.Dir);
    REQUIRE(store);
    (*store)->RegisterFamily(MakePatternFamily(types));

    // Twice: an empty record per marked entity per capture would grow the family forever.
    (*store)->CaptureScene(*scene);
    (*store)->CaptureScene(*scene);

    CHECK((*store)->GetRecordCount() == 0);
    CHECK_FALSE((*store)->Read(PatternFamily, StoreKey{.Lo = 3, .Hi = 0}).has_value());
}

TEST_CASE("a component absent from the entity is not captured")
{
    const TempSlot slot;
    TypeRegistry types = MakeRegistry();
    const Unique<Scene> scene = Scene::Create(types);
    const Entity entity = SpawnTagged(*scene, 4);
    scene->Add<PatternAlpha>(entity, PatternAlpha{.Value = 11});

    Result<Unique<Store>> store = Store::Open(slot.Dir);
    REQUIRE(store);
    (*store)->RegisterFamily(MakePatternFamily(types));
    (*store)->CaptureScene(*scene);

    const optional<StoreRecord> record = (*store)->Read(PatternFamily, StoreKey{.Lo = 4, .Hi = 0});
    REQUIRE(record.has_value());
    REQUIRE(record->Components.size() == 1);
    CHECK(record->Components.front().Type == TypeIdOf<PatternAlpha>());
}

TEST_CASE("rehydrate adds a stored component the claimant does not carry")
{
    const TempSlot slot;
    TypeRegistry types = MakeRegistry();
    Result<Unique<Store>> store = Store::Open(slot.Dir);
    REQUIRE(store);
    (*store)->RegisterFamily(MakePatternFamily(types));

    {
        const Unique<Scene> source = Scene::Create(types);
        const Entity entity = SpawnTagged(*source, 8);
        source->Add<PatternAlpha>(entity, PatternAlpha{.Value = 77});
        (*store)->CaptureScene(*source);
    }

    // The claimant carries the marker alone — the freshly-built-entity case.
    const Unique<Scene> fresh = Scene::Create(types);
    const Entity restored = SpawnTagged(*fresh, 8);
    REQUIRE(fresh->TryGet<PatternAlpha>(restored) == nullptr);

    (*store)->RehydrateScene(*fresh);

    const PatternAlpha* const alpha = fresh->TryGet<PatternAlpha>(restored);
    REQUIRE(alpha != nullptr);
    CHECK(alpha->Value == 77);
}

TEST_CASE("a component type absent from a record leaves the live component untouched")
{
    const TempSlot slot;
    TypeRegistry types = MakeRegistry();
    Result<Unique<Store>> store = Store::Open(slot.Dir);
    REQUIRE(store);
    (*store)->RegisterFamily(MakePatternFamily(types));

    {
        const Unique<Scene> source = Scene::Create(types);
        const Entity entity = SpawnTagged(*source, 9);
        source->Add<PatternAlpha>(entity, PatternAlpha{.Value = 1});
        (*store)->CaptureScene(*source);
    }

    const Unique<Scene> fresh = Scene::Create(types);
    const Entity restored = SpawnTagged(*fresh, 9);
    fresh->Add<PatternBeta>(restored, PatternBeta{.Weight = 55});

    (*store)->RehydrateScene(*fresh);

    const PatternBeta* const beta = fresh->TryGet<PatternBeta>(restored);
    REQUIRE(beta != nullptr);
    CHECK(beta->Weight == 55);
}

TEST_CASE("a stored blob the family captures no component for is skipped and logged once")
{
    const TempSlot slot;
    TypeRegistry types = MakeRegistry();
    Result<Unique<Store>> store = Store::Open(slot.Dir);
    REQUIRE(store);
    (*store)->RegisterFamily(MakePatternFamily(types));

    StoreRecord record{.CapturedAtWall = Store::WallClockSeconds()};
    ComponentBlob alphaBlob{.Type = TypeIdOf<PatternAlpha>()};
    const PatternAlpha alpha{.Value = 21};
    WriteFields(alphaBlob.Bytes, &alpha, types.Info(TypeIdOf<PatternAlpha>()), types);
    ComponentBlob strayBlob{.Type = TypeIdOf<PatternStray>()};
    const PatternStray stray{.Noise = 3};
    WriteFields(strayBlob.Bytes, &stray, types.Info(TypeIdOf<PatternStray>()), types);
    record.Components.push_back(std::move(strayBlob));
    record.Components.push_back(std::move(alphaBlob));
    (*store)->Write(PatternFamily, StoreKey{.Lo = 12, .Hi = 0}, std::move(record));

    const Unique<Scene> fresh = Scene::Create(types);
    const Entity restored = SpawnTagged(*fresh, 12);

    usize warnings = 0;
    {
        const WarnCapture capture;
        (*store)->RehydrateScene(*fresh);
        (*store)->RehydrateScene(*fresh);
        warnings = capture.Warnings.size();

        // The one line names the unmatched type, so the drift is actionable.
        REQUIRE(warnings == 1);
        CHECK(capture.Warnings.front().find(fmt::format("{:016X}", TypeIdOf<PatternStray>())) !=
              string::npos);
    }

    // The unmatched blob is skipped, not fatal: the record's other blobs still apply.
    const PatternAlpha* const applied = fresh->TryGet<PatternAlpha>(restored);
    REQUIRE(applied != nullptr);
    CHECK(applied->Value == 21);
    CHECK(fresh->TryGet<PatternStray>(restored) == nullptr);
}

TEST_CASE("only entities whose key matches a stored record rehydrate")
{
    const TempSlot slot;
    TypeRegistry types = MakeRegistry();
    Result<Unique<Store>> store = Store::Open(slot.Dir);
    REQUIRE(store);
    (*store)->RegisterFamily(MakePatternFamily(types));

    {
        const Unique<Scene> source = Scene::Create(types);
        const Entity entity = SpawnTagged(*source, 1);
        source->Add<PatternAlpha>(entity, PatternAlpha{.Value = 100});
        (*store)->CaptureScene(*source);
    }

    const Unique<Scene> fresh = Scene::Create(types);
    const Entity matching = SpawnTagged(*fresh, 1);
    const Entity other = SpawnTagged(*fresh, 2);
    (*store)->RehydrateScene(*fresh);

    REQUIRE(fresh->TryGet<PatternAlpha>(matching) != nullptr);
    CHECK(fresh->TryGet<PatternAlpha>(matching)->Value == 100);
    CHECK(fresh->TryGet<PatternAlpha>(other) == nullptr);
}

TEST_CASE("where several entities claim one key the first wins")
{
    const TempSlot slot;
    TypeRegistry types = MakeRegistry();
    Result<Unique<Store>> store = Store::Open(slot.Dir);
    REQUIRE(store);
    (*store)->RegisterFamily(MakePatternFamily(types));

    {
        const Unique<Scene> source = Scene::Create(types);
        const Entity entity = SpawnTagged(*source, 6);
        source->Add<PatternAlpha>(entity, PatternAlpha{.Value = 64});
        (*store)->CaptureScene(*source);
    }

    const Unique<Scene> fresh = Scene::Create(types);
    const Entity first = SpawnTagged(*fresh, 6);
    const Entity second = SpawnTagged(*fresh, 6);
    (*store)->RehydrateScene(*fresh);

    REQUIRE(fresh->TryGet<PatternAlpha>(first) != nullptr);
    CHECK(fresh->TryGet<PatternAlpha>(first)->Value == 64);
    CHECK(fresh->TryGet<PatternAlpha>(second) == nullptr);
}

TEST_CASE("the singleton reads nullopt when its record or blob is absent")
{
    const TempSlot slot;
    const TypeRegistry types = MakeRegistry();
    Result<Unique<Store>> store = Store::Open(slot.Dir);
    REQUIRE(store);
    (*store)->RegisterFamily(SingletonFamily(SingletonTestFamily, "settings"));

    CHECK_FALSE(ReadSingleton<PatternAlpha>(**store, SingletonTestFamily, types).has_value());

    WriteSingleton(**store, SingletonTestFamily, PatternBeta{.Weight = 2}, types);
    CHECK_FALSE(ReadSingleton<PatternAlpha>(**store, SingletonTestFamily, types).has_value());
}

TEST_CASE("the singleton's read-modify-write preserves a foreign blob")
{
    const TempSlot slot;
    const TypeRegistry types = MakeRegistry();
    {
        Result<Unique<Store>> store = Store::Open(slot.Dir);
        REQUIRE(store);
        (*store)->RegisterFamily(SingletonFamily(SingletonTestFamily, "settings"));

        WriteSingleton(**store, SingletonTestFamily, PatternAlpha{.Value = 3}, types);
        WriteSingleton(**store, SingletonTestFamily, PatternBeta{.Weight = 4}, types);
        // Replacing one type must leave the other's blob in the record untouched.
        WriteSingleton(**store, SingletonTestFamily, PatternAlpha{.Value = 5}, types);

        const optional<StoreRecord> record =
            (*store)->Read(SingletonTestFamily, SingletonRecordKey);
        REQUIRE(record.has_value());
        CHECK(record->Components.size() == 2);
        REQUIRE((*store)->Flush());
    }

    Result<Unique<Store>> reopened = Store::Open(slot.Dir);
    REQUIRE(reopened);
    (*reopened)->RegisterFamily(SingletonFamily(SingletonTestFamily, "settings"));

    const optional<PatternAlpha> alpha =
        ReadSingleton<PatternAlpha>(**reopened, SingletonTestFamily, types);
    const optional<PatternBeta> beta =
        ReadSingleton<PatternBeta>(**reopened, SingletonTestFamily, types);
    REQUIRE(alpha.has_value());
    REQUIRE(beta.has_value());
    CHECK(alpha->Value == 5);
    CHECK(beta->Weight == 4);
}
