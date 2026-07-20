// Level cook test: cooks a *.level.json through the LevelImporter against the reflected
// hello-triangle registry (loaded module: builtins + Spinner types, the game systems in the
// SystemRegistry) and checks the CookedLevelHeader (world id, system count, record sizes),
// the system-id array, and that the gameMode/render records round-trip back through ReadFields.
// Also covers each validation failure (unknown system id, world id resolving to a non-prefab,
// a malformed game-mode field), the no-module error, and the tolerant decode of a pre-change
// game-mode record carrying a field the schema no longer declares.

#include <cstring>
#include <filesystem>
#include "support/TempPath.h"
#include <fstream>
#include <random>

#include <doctest/doctest.h>
#include <fmt/format.h>

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/HexId.h>
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

        // Unique per call: ctest runs each case as its own process in parallel, and a
        // shared fixed name lets concurrent cases cook over and delete each other's archive.
        std::random_device rng;
        const path outArchive =
            Veng::TestSupport::TempDir() / fmt::format("veng_cooker_level_{:08x}.vengpack", rng());

        const VoidResult cookResult =
            cooker.CookPack(packJson, outArchive, refs, &module.Types, &module.Systems);
        if (!cookResult.has_value())
        {
            return std::unexpected(cookResult.error());
        }

        const Result<ArchiveReader> reader = ArchiveReader::Open(outArchive);
        if (!reader.has_value())
        {
            std::filesystem::remove(outArchive);
            return std::unexpected(reader.error());
        }

        const optional<ArchiveEntry> entry = reader->Find(levelId);
        if (!entry.has_value())
        {
            std::filesystem::remove(outArchive);
            return std::unexpected(string("level entry missing from archive"));
        }
        vector<u8> blob(entry->Blob.begin(), entry->Blob.end());
        std::filesystem::remove(outArchive);
        return blob;
    }

    // Writes a level JSON + a one-entry level pack into a temp dir, returning the pack path.
    path WriteLevelPack(const string& name, const json& level)
    {
        const path dir = Veng::TestSupport::TempDir();
        const path levelPath = dir / (name + ".level.json");
        const path packPath = dir / (name + ".pack.json");

        std::ofstream(levelPath) << level.dump();

        json pack;
        pack["version"] = 1;
        json asset;
        asset["id"] = FormatHexId(7777);
        asset["type"] = "Level";
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
        level["world"] = FormatHexId(WorldPrefabId);
        level["systems"] =
            json::array({FormatHexId(SpawnPlayerRuleId), FormatHexId(ControlSystemId),
                         FormatHexId(MovementSystemId)});
        level["gameMode"] = {{"PlayerPrefab", FormatHexId(PlayerPrefabId)}};
        level["render"] = {{"Exposure", 2.5},         {"Bloom", true},
                           {"BloomIntensity", 1.5},   {"DepthOfField", true},
                           {"DofFocusDistance", 3.5}, {"DofRingCount", 6}};
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
    // The depth-of-field fields ride the same name-keyed record — no cooked-level version bump.
    CHECK(render.DepthOfField == true);
    CHECK(render.DofFocusDistance == doctest::Approx(3.5f));
    CHECK(render.DofRingCount == 6);

    CHECK(static_cast<usize>((cursor - blob.data()) + header.RenderRecordBytes) == blob.size());
}

TEST_CASE("level cook: an unknown system id is a located error")
{
    const LoadedModuleTypes module = LoadRegistry();

    json level = SampleLevel();
    level["systems"] =
        json::array({FormatHexId(SpawnPlayerRuleId), FormatHexId(0xDEADBEEFCAFEF00DULL)});
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
    level["world"] = FormatHexId(1001);
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
    level["gameMode"] = {{"PlayerPrefab", 12345}};
    const path packJson = WriteLevelPack("level_bad_field", level);
    const path refs[] = {path(VENG_HT_ASSETS_DIR) / "sample.vengpack.json"};

    const Result<vector<u8>> blob = CookLevel(packJson, module, refs, AssetId{7777});
    REQUIRE_FALSE(blob.has_value());
    CHECK(blob.error().find("PlayerPrefab") != string::npos);
}

TEST_CASE("level cook: a pre-change game-mode record decodes tolerantly within the same version")
{
    // A blob cooked before a GameModeConfig field was removed still carries that field's record;
    // the name-keyed decode skips the unknown record and fills the surviving fields, so old cooked
    // levels load without a version bump. Hand-assemble such a record: the current PlayerPrefab
    // plus a stale field the schema no longer declares.
    const LoadedModuleTypes module = LoadRegistry();

    const auto appendU32 = [](vector<u8>& out, const u32 value)
    {
        const auto* p = reinterpret_cast<const u8*>(&value);
        out.insert(out.end(), p, p + sizeof(value));
    };

    vector<u8> record;
    appendU32(record, 2); // record count
    const string prefabName = "PlayerPrefab";
    appendU32(record, static_cast<u32>(prefabName.size()));
    record.insert(record.end(), prefabName.begin(), prefabName.end());
    appendU32(record, sizeof(u64));
    const u64 prefabId = PlayerPrefabId;
    const auto* idBytes = reinterpret_cast<const u8*>(&prefabId);
    record.insert(record.end(), idBytes, idBytes + sizeof(prefabId));
    const string staleName = "StaleModeKnob"; // a field the schema no longer declares
    appendU32(record, static_cast<u32>(staleName.size()));
    record.insert(record.end(), staleName.begin(), staleName.end());
    appendU32(record, sizeof(i32));
    const i32 staleValue = 3;
    const auto* staleBytes = reinterpret_cast<const u8*>(&staleValue);
    record.insert(record.end(), staleBytes, staleBytes + sizeof(staleValue));

    GameModeConfig gameMode;
    REQUIRE(
        ReadFields(record, &gameMode, module.Types.Info(TypeIdOf<GameModeConfig>()), module.Types)
            .has_value());
    CHECK(gameMode.PlayerPrefab.Id().Value == PlayerPrefabId);
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

    const path outArchive = Veng::TestSupport::TempDir() / "veng_cooker_level_nm.vengpack";
    const VoidResult result = cooker.CookPack(packJson, outArchive);
    REQUIRE_FALSE(result.has_value());
    CHECK(result.error().find("level cooking requires --module") != string::npos);
}
