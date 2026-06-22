// Level loader + LoadInto: the runtime "start the game" path. Builds a Level over a
// resident world prefab and an ordered SystemId set, then LoadInto-s it and checks the
// loader spawned the world, seeded a Playing Session from the game-mode config, and built
// the simulation from exactly the named system set (proven by ticking it once and observing
// only the named systems run). Also pins that a version-mismatched cooked blob loads as
// AssetError::Corrupt rather than aborting.
//
// It lives in the GPU band only because LoadInto/SpawnInto take an AssetManager&, which
// requires a Context; the assertions touch no device.

#include <cstring>

#include <doctest/doctest.h>

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/Level.h>
#include <Veng/Asset/Prefab.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneSimulation.h>
#include <Veng/Scene/SceneSystem.h>
#include <Veng/Scene/SystemRegistry.h>

#include <gpu/fixture.h>

using namespace Veng;

namespace
{
    // Two counting systems whose updates record into a shared log, so a LoadInto-built
    // simulation proves it ran exactly the named ids.
    int g_RanA = 0;
    int g_RanB = 0;

    struct LevelSystemA final : SceneSystem
    {
        void OnUpdate(Scene&, f32, const SystemContext&) override { ++g_RanA; }
    };

    struct LevelSystemB final : SceneSystem
    {
        void OnUpdate(Scene&, f32, const SystemContext&) override { ++g_RanB; }
    };
}

namespace Veng
{
    template <>
    struct VengSystem<LevelSystemA>
    {
        static constexpr SystemId Id = 0x1E7E10A000000001ULL;
        static string Name() { return "LevelSystemA"; }
    };

    template <>
    struct VengSystem<LevelSystemB>
    {
        static constexpr SystemId Id = 0x1E7E10A000000002ULL;
        static string Name() { return "LevelSystemB"; }
    };
}

namespace
{
    struct LevelFixture : Veng::Test::GpuFixture
    {
        Unique<AssetManager> Assets;
        SystemRegistry Systems;

        LevelFixture()
        {
            RegisterBuiltinTypes(Types);
            Assets = CreateUnique<AssetManager>(Context, Tasks, Types);
            Systems.Register<LevelSystemA>();
            Systems.Register<LevelSystemB>();
            g_RanA = 0;
            g_RanB = 0;
        }

        // A one-entity world prefab adopted as a resident handle (no archive).
        AssetHandle<Prefab> MakeWorld()
        {
            const Name rootName{"World Root"};
            Prefab::Component name;
            name.Type = Types.IdOf<Name>();
            WriteFields(name.Record, &rootName, Types.Info(name.Type), Types);

            vector<Prefab::PrefabEntity> entities;
            entities.push_back({{std::move(name)}});
            return Assets->Adopt<Prefab>(Prefab::Create(std::move(entities), {}));
        }
    };
}

TEST_CASE_FIXTURE(LevelFixture,
                  "LoadInto spawns the world, seeds a Playing Session, and runs the named systems")
{
    const AssetHandle<Prefab> world = MakeWorld();

    GameModeConfig gameMode;
    gameMode.ScoreToWin = 7;

    // Name only LevelSystemB, in a one-element set, to prove LoadInto builds exactly the
    // level's set (SystemA, though registered, must not run).
    const vector<SystemId> systems = {SystemIdOf<LevelSystemB>()};

    const Ref<Level> level = Level::Create(world, systems, gameMode, LevelRenderSettings{});
    LevelInstance instance = level->LoadInto(*Assets, Systems);

    REQUIRE(instance.World != nullptr);
    REQUIRE(instance.Simulation != nullptr);

    // The world prefab's single root spawned.
    bool sawWorldRoot = false;
    for (auto [entity, n] : instance.World->View<Name>())
    {
        if (n.Value == "World Root")
        {
            sawWorldRoot = true;
        }
    }
    CHECK(sawWorldRoot);

    // The loader seeded one Session entity carrying a Playing Session plus the game-mode config.
    Entity session = Entity::Null;
    instance.World->Each<Session, GameModeConfig>([&](Entity e, Session&, GameModeConfig&)
                                                  { session = e; });
    REQUIRE(session != Entity::Null);
    CHECK(instance.World->Get<Session>(session).Phase == SessionPhase::Playing);
    CHECK(instance.World->Get<GameModeConfig>(session).ScoreToWin == 7);

    // Ticking the simulation runs exactly the named system (B), not the unnamed one (A).
    // The counting systems never read Input, so a never-dereferenced placeholder lvalue
    // satisfies the SystemContext aggregate without a Window-bound Input.
    alignas(16) unsigned char inputBytes[64]{};
    const SystemContext context{.Assets = *Assets, .Input = *reinterpret_cast<Input*>(inputBytes)};
    instance.Simulation->Update(*instance.World, 0.016f, context);
    CHECK(g_RanA == 0);
    CHECK(g_RanB == 1);
}

TEST_CASE_FIXTURE(LevelFixture, "A version-mismatched cooked level loads as AssetError::Corrupt")
{
    const AssetId levelId{0x5111A2C033B47EE0ULL};

    CookedLevelHeader header{};
    header.Version = CookedLevelVersion + 1; // stale/foreign
    header.WorldPrefabId = 0;
    header.SystemCount = 0;
    header.GameModeRecordBytes = 0;
    header.RenderRecordBytes = 0;

    vector<u8> blob(sizeof(header));
    std::memcpy(blob.data(), &header, sizeof(header));

    ArchiveWriter writer;
    writer.Add(levelId, AssetType::Level, blob);
    const MountHandle mount = Assets->MountMemory(writer.Build(), "stale_level");

    const AssetResult<AssetHandle<Level>> result = Assets->LoadSync<Level>(levelId);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().Kind == AssetError::Corrupt);
    CHECK(result.error().Id == levelId);
}
