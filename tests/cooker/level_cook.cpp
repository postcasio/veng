// Level cook test: cooks a *.level.json through the LevelImporter against the reflected
// hello-triangle registry (loaded module: builtins + Spinner types, the game systems in the
// SystemRegistry) and checks the CookedLevelHeader (world id, system count, record sizes),
// the system-id array, and that the gameMode/render records round-trip back through ReadFields.
// Also covers each validation failure (unknown system id, world id resolving to a non-prefab,
// a malformed game-mode field) and the no-module error.

#include <cstring>
#include <filesystem>
#include <fstream>

#include <doctest/doctest.h>

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Cook/ModuleTypes.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Components.h>

using namespace Veng;
using namespace Veng::Cook;

namespace
{
    // The hello-triangle module's system ids (mirrors the VE_SYSTEM literals in the sample
    // and the engine), so the level fixture names ids that resolve against the catalog.
    constexpr SystemId SpawnPlayerRuleId = 0x70CCE23C99D1C3A1ULL;
    constexpr SystemId ControlSystemId = 0x1C2F5C03357C19B2ULL;
    constexpr SystemId MovementSystemId = 0x3C012FCD7D93E513ULL;

    // The sample world prefab id (resolves to a prefab in the reference pack).
    constexpr u64 WorldPrefabId = 11611391513566245589ULL;
    // The sample player prefab id (the game-mode config's PlayerPrefab).
    constexpr u64 PlayerPrefabId = 13493236524696338033ULL;

    LoadedModuleTypes LoadRegistry()
    {
        Result<LoadedModuleTypes> loaded = LoadModuleTypes(path{VENG_HELLO_TRIANGLE_MODULE_PATH});
        REQUIRE(loaded.has_value());
        return std::move(*loaded);
    }

    // Cooks a level pack with the level importer + the reflected registry, returning the
    // level blob bytes (or the located error).
    Result<vector<u8>> CookLevel(const path& packJson, const LoadedModuleTypes& module,
                                 std::span<const path> refs, AssetId levelId)
    {
        Cooker cooker;
        RegisterBuiltinImporters(cooker);
        RegisterPrefabImporter(cooker);
        RegisterLevelImporter(cooker);

        const path outArchive =
            std::filesystem::temp_directory_path() / "veng_cooker_level.vengpack";

        const VoidResult cookResult =
            cooker.CookPack(packJson, outArchive, refs, &module.Types, &module.Systems);
        if (!cookResult.has_value())
        {
            return std::unexpected(cookResult.error());
        }

        const Result<ArchiveReader> reader = ArchiveReader::Open(outArchive);
        if (!reader.has_value())
        {
            return std::unexpected(reader.error());
        }

        const optional<ArchiveEntry> entry = reader->Find(levelId);
        if (!entry.has_value())
        {
            return std::unexpected(string("level entry missing from archive"));
        }
        return vector<u8>(entry->Blob.begin(), entry->Blob.end());
    }

    // Writes a level JSON + a one-entry level pack into a temp dir, returning the pack path.
    path WriteLevelPack(const string& name, const json& level)
    {
        const path dir = std::filesystem::temp_directory_path();
        const path levelPath = dir / (name + ".level.json");
        const path packPath = dir / (name + ".pack.json");

        std::ofstream(levelPath) << level.dump();

        json pack;
        pack["version"] = 1;
        json asset;
        asset["id"] = 7777;
        asset["type"] = "level";
        asset["source"] = levelPath.filename().string();
        pack["assets"] = json::array({asset});
        std::ofstream(packPath) << pack.dump();

        return packPath;
    }

    // A well-formed sample level: the world prefab, three named systems, a game-mode config,
    // and a render subset.
    json SampleLevel()
    {
        json level;
        level["world"] = WorldPrefabId;
        level["systems"] = json::array({SpawnPlayerRuleId, ControlSystemId, MovementSystemId});
        level["gameMode"] = {{"PlayerPrefab", PlayerPrefabId}, {"ScoreToWin", 3}};
        level["render"] = {{"Exposure", 2.5}, {"Bloom", true}, {"BloomIntensity", 1.5}};
        return level;
    }
}

TEST_CASE("level cook: happy path — header, system ids, config record round-trip")
{
    const LoadedModuleTypes module = LoadRegistry();

    const path packJson = WriteLevelPack("level_happy", SampleLevel());
    const path refs[] = {path(VENG_HT_ASSETS_DIR) / "sample.vengpack.json"};

    const Result<vector<u8>> blobResult = CookLevel(packJson, module, refs, AssetId{7777});
    REQUIRE_MESSAGE(blobResult.has_value(),
                    "cook failed: ", blobResult ? string{} : blobResult.error());

    const vector<u8>& blob = *blobResult;
    REQUIRE(blob.size() >= sizeof(CookedLevelHeader));

    CookedLevelHeader header{};
    std::memcpy(&header, blob.data(), sizeof(header));

    CHECK(header.Version == CookedLevelVersion);
    CHECK(header.WorldPrefabId == WorldPrefabId);
    CHECK(header.SystemCount == 3);

    const u8* cursor = blob.data() + sizeof(CookedLevelHeader);

    // The system ids follow the header, in authored order.
    vector<u64> systems(header.SystemCount);
    std::memcpy(systems.data(), cursor, header.SystemCount * sizeof(u64));
    cursor += header.SystemCount * sizeof(u64);
    REQUIRE(systems.size() == 3);
    CHECK(systems[0] == SpawnPlayerRuleId);
    CHECK(systems[1] == ControlSystemId);
    CHECK(systems[2] == MovementSystemId);

    // The game-mode record round-trips back to the authored values.
    const std::span<const u8> gameModeRecord(cursor, header.GameModeRecordBytes);
    cursor += header.GameModeRecordBytes;
    GameModeConfig gameMode;
    REQUIRE(ReadFields(gameModeRecord, &gameMode, module.Types.Info(TypeIdOf<GameModeConfig>()),
                       module.Types)
                .has_value());
    CHECK(gameMode.PlayerPrefab.Id().Value == PlayerPrefabId);
    CHECK(gameMode.ScoreToWin == 3);

    // The render record round-trips, and an omitted field keeps its default.
    const std::span<const u8> renderRecord(cursor, header.RenderRecordBytes);
    LevelRenderSettings render;
    REQUIRE(ReadFields(renderRecord, &render, module.Types.Info(TypeIdOf<LevelRenderSettings>()),
                       module.Types)
                .has_value());
    CHECK(render.Exposure == doctest::Approx(2.5f));
    CHECK(render.Bloom == true);
    CHECK(render.BloomIntensity == doctest::Approx(1.5f));
    // Shadows/AO were omitted from the sample render block and keep their struct defaults.
    CHECK(render.Shadows == true);
    CHECK(render.AO == true);

    CHECK(static_cast<usize>((cursor - blob.data()) + header.RenderRecordBytes) == blob.size());
}

TEST_CASE("level cook: an unknown system id is a located error")
{
    const LoadedModuleTypes module = LoadRegistry();

    json level = SampleLevel();
    level["systems"] = json::array({SpawnPlayerRuleId, 0xDEADBEEFCAFEF00DULL});
    const path packJson = WriteLevelPack("level_unknown_system", level);
    const path refs[] = {path(VENG_HT_ASSETS_DIR) / "sample.vengpack.json"};

    const Result<vector<u8>> blob = CookLevel(packJson, module, refs, AssetId{7777});
    REQUIRE_FALSE(blob.has_value());
    CHECK(blob.error().find("system id") != string::npos);
    CHECK(blob.error().find("not registered") != string::npos);
}

TEST_CASE("level cook: a world id resolving to a non-prefab is a located error")
{
    const LoadedModuleTypes module = LoadRegistry();

    // Id 1001 resolves to a texture in the sample pack, not a prefab.
    json level = SampleLevel();
    level["world"] = 1001;
    const path packJson = WriteLevelPack("level_world_nonprefab", level);
    const path refs[] = {path(VENG_HT_ASSETS_DIR) / "sample.vengpack.json"};

    const Result<vector<u8>> blob = CookLevel(packJson, module, refs, AssetId{7777});
    REQUIRE_FALSE(blob.has_value());
    CHECK(blob.error().find("not a prefab") != string::npos);
}

TEST_CASE("level cook: a malformed game-mode field is a located error")
{
    const LoadedModuleTypes module = LoadRegistry();

    json level = SampleLevel();
    level["gameMode"] = {{"ScoreToWin", "not a number"}};
    const path packJson = WriteLevelPack("level_bad_field", level);
    const path refs[] = {path(VENG_HT_ASSETS_DIR) / "sample.vengpack.json"};

    const Result<vector<u8>> blob = CookLevel(packJson, module, refs, AssetId{7777});
    REQUIRE_FALSE(blob.has_value());
    CHECK(blob.error().find("ScoreToWin") != string::npos);
}

TEST_CASE("level cook: an unknown game-mode field is a located error")
{
    const LoadedModuleTypes module = LoadRegistry();

    json level = SampleLevel();
    level["gameMode"] = {{"Nonexistent", 1}};
    const path packJson = WriteLevelPack("level_unknown_field", level);
    const path refs[] = {path(VENG_HT_ASSETS_DIR) / "sample.vengpack.json"};

    const Result<vector<u8>> blob = CookLevel(packJson, module, refs, AssetId{7777});
    REQUIRE_FALSE(blob.has_value());
    CHECK(blob.error().find("Nonexistent") != string::npos);
}

TEST_CASE("level cook: cooking a level with no --module is the requires-module error")
{
    const path packJson = WriteLevelPack("level_no_module", SampleLevel());

    Cooker cooker;
    RegisterBuiltinImporters(cooker);
    RegisterPrefabImporter(cooker);
    RegisterLevelImporter(cooker);

    const path outArchive =
        std::filesystem::temp_directory_path() / "veng_cooker_level_nm.vengpack";
    const VoidResult result = cooker.CookPack(packJson, outArchive);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("level cooking requires --module") != string::npos);
}
