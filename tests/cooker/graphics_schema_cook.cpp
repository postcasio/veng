// Graphics-schema cook test: cooks a *.gfxschema.json through the SettingsSchemaImporter and checks
// the CookedSettingsSchemaHeader plus that the schema record round-trips back through ReadFields.
// Also covers each validation failure — a duplicate setting id, a discrete setting with no options,
// an out-of-range scalar default, a DefaultPreset naming no preset, a preset entry naming a missing
// setting, and a preset entry naming a non-render_scale built-in. A graphics schema needs no
// --module (it references only engine builtins), so the cook runs with a builtin-only registry.

#include <cstring>
#include <filesystem>
#include <fstream>
#include <random>

#include "support/TempPath.h"

#include <doctest/doctest.h>
#include <fmt/format.h>

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/HexId.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Render/GraphicsSchema.h>
#include <Veng/Scene/BuiltinTypes.h>

using namespace Veng;
using namespace Veng::Cook;

namespace
{
    // Cooks a one-entry graphics-schema pack and returns the cooked blob bytes (or the error).
    Result<vector<u8>> CookSchema(const path& packJson, AssetId schemaId)
    {
        Cooker cooker;
        RegisterBuiltinImporters(cooker);

        std::random_device rng;
        const path outArchive = Veng::TestSupport::TempDir() /
                                fmt::format("veng_cooker_gfxschema_{:08x}.vengpack", rng());

        const VoidResult cookResult = cooker.CookPack(packJson, outArchive);
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
        const optional<ArchiveEntry> entry = reader->Find(schemaId);
        if (!entry.has_value())
        {
            std::filesystem::remove(outArchive);
            return std::unexpected(string("graphics schema entry missing from archive"));
        }
        vector<u8> blob(entry->Blob.begin(), entry->Blob.end());
        std::filesystem::remove(outArchive);
        return blob;
    }

    // Writes a gfxschema JSON + a one-entry pack into a temp dir, returning the pack path.
    path WriteSchemaPack(const string& name, const json& schema)
    {
        const path dir = Veng::TestSupport::TempDir();
        const path schemaPath = dir / (name + ".gfxschema.json");
        const path packPath = dir / (name + ".pack.json");

        std::ofstream(schemaPath) << schema.dump();

        json pack;
        pack["version"] = 1;
        json asset;
        asset["id"] = FormatHexId(4242);
        asset["type"] = "GraphicsSchema";
        asset["source"] = schemaPath.filename().string();
        pack["assets"] = json::array({asset});
        std::ofstream(packPath) << pack.dump();

        return packPath;
    }

    // A well-formed sample: a discrete shadows setting and a scalar fov, plus Low/High presets over
    // both and the render-scale built-in.
    json SampleSchema()
    {
        json schema;
        schema["Categories"] = json::array(
            {{{"Id", "quality"},
              {"Label", "Quality"},
              {"Settings",
               json::array({{{"Id", "shadows"},
                             {"Label", "Shadows"},
                             {"Kind", "Discrete"},
                             {"Options", json::array({{{"Id", "off"}, {"Label", "Off"}},
                                                      {{"Id", "high"}, {"Label", "High"}}})},
                             {"DefaultOption", 1}}})}},
             {{"Id", "camera"},
              {"Label", "Camera"},
              {"Settings", json::array({{{"Id", "fov"},
                                         {"Label", "Field of View"},
                                         {"Kind", "Scalar"},
                                         {"Min", 60.0},
                                         {"Max", 110.0},
                                         {"Step", 1.0},
                                         {"DefaultValue", 90.0}}})}}});
        schema["Presets"] = json::array(
            {{{"Id", "low"},
              {"Label", "Low"},
              {"Entries", json::array({{{"SettingId", "shadows"}, {"OptionId", "off"}},
                                       {{"SettingId", "render_scale"}, {"ScalarValue", 0.75}}})}},
             {{"Id", "high"},
              {"Label", "High"},
              {"Entries", json::array({{{"SettingId", "shadows"}, {"OptionId", "high"}},
                                       {{"SettingId", "render_scale"}, {"ScalarValue", 1.0}}})}}});
        schema["DefaultPreset"] = "high";
        return schema;
    }

    // Cooks `schema` and returns the error string, or empty on unexpected success.
    string CookExpectingError(const string& name, const json& schema)
    {
        const path packJson = WriteSchemaPack(name, schema);
        const Result<vector<u8>> result = CookSchema(packJson, AssetId{4242});
        return result.has_value() ? string{} : result.error();
    }
}

TEST_CASE("graphics schema cook: happy path — header + schema record round-trip")
{
    const path packJson = WriteSchemaPack("gfxschema_happy", SampleSchema());

    const Result<vector<u8>> blobResult = CookSchema(packJson, AssetId{4242});
    REQUIRE_MESSAGE(blobResult.has_value(),
                    "cook failed: ", blobResult ? string{} : blobResult.error());

    const vector<u8>& blob = *blobResult;
    REQUIRE(blob.size() >= sizeof(CookedSettingsSchemaHeader));

    CookedSettingsSchemaHeader header{};
    std::memcpy(&header, blob.data(), sizeof(header));
    CHECK(header.Version == CookedSettingsSchemaVersion);
    REQUIRE(blob.size() == sizeof(CookedSettingsSchemaHeader) + header.RecordBytes);

    TypeRegistry registry;
    RegisterBuiltinTypes(registry);
    const std::span<const u8> record(blob.data() + sizeof(CookedSettingsSchemaHeader),
                                     header.RecordBytes);
    GraphicsSchemaData data;
    REQUIRE(ReadFields(record, &data, registry.Info(TypeIdOf<GraphicsSchemaData>()), registry)
                .has_value());

    REQUIRE(data.Categories.size() == 2);
    CHECK(data.Categories[0].Settings[0].Id == "shadows");
    CHECK(data.Categories[0].Settings[0].Kind == GraphicsSettingKind::Discrete);
    CHECK(data.Categories[0].Settings[0].DefaultOption == 1);
    CHECK(data.Categories[1].Settings[0].Kind == GraphicsSettingKind::Scalar);
    CHECK(data.Categories[1].Settings[0].DefaultValue == doctest::Approx(90.0f));
    REQUIRE(data.Presets.size() == 2);
    CHECK(data.Presets[1].Entries[1].SettingId == "render_scale");
    CHECK(data.DefaultPreset == "high");
}

TEST_CASE("graphics schema cook: a duplicate setting id is a located error")
{
    json schema = SampleSchema();
    // Re-declare 'shadows' in the second category.
    schema["Categories"][1]["Settings"].push_back(
        {{"Id", "shadows"},
         {"Label", "Dup"},
         {"Kind", "Discrete"},
         {"Options", json::array({{{"Id", "off"}, {"Label", "Off"}}})},
         {"DefaultOption", 0}});
    const string error = CookExpectingError("gfxschema_dupid", schema);
    REQUIRE_FALSE(error.empty());
    CHECK(error.find("shadows") != string::npos);
    CHECK(error.find("more than once") != string::npos);
}

TEST_CASE("graphics schema cook: a discrete setting with no options is a located error")
{
    json schema = SampleSchema();
    schema["Categories"][0]["Settings"][0]["Options"] = json::array();
    schema["Categories"][0]["Settings"][0]["DefaultOption"] = 0;
    const string error = CookExpectingError("gfxschema_noopts", schema);
    REQUIRE_FALSE(error.empty());
    CHECK(error.find("no options") != string::npos);
}

TEST_CASE("graphics schema cook: an out-of-range scalar default is a located error")
{
    json schema = SampleSchema();
    schema["Categories"][1]["Settings"][0]["DefaultValue"] = 200.0; // above Max 110
    const string error = CookExpectingError("gfxschema_badscalar", schema);
    REQUIRE_FALSE(error.empty());
    CHECK(error.find("DefaultValue") != string::npos);
}

TEST_CASE("graphics schema cook: a discrete DefaultOption out of range is a located error")
{
    json schema = SampleSchema();
    schema["Categories"][0]["Settings"][0]["DefaultOption"] = 9;
    const string error = CookExpectingError("gfxschema_badoption", schema);
    REQUIRE_FALSE(error.empty());
    CHECK(error.find("DefaultOption") != string::npos);
}

TEST_CASE("graphics schema cook: a DefaultPreset naming no preset is a located error")
{
    json schema = SampleSchema();
    schema["DefaultPreset"] = "ultra";
    const string error = CookExpectingError("gfxschema_badpreset", schema);
    REQUIRE_FALSE(error.empty());
    CHECK(error.find("ultra") != string::npos);
}

TEST_CASE("graphics schema cook: a preset entry naming a missing setting is a located error")
{
    json schema = SampleSchema();
    schema["Presets"][0]["Entries"].push_back({{"SettingId", "ssr"}, {"OptionId", "on"}});
    const string error = CookExpectingError("gfxschema_missingsetting", schema);
    REQUIRE_FALSE(error.empty());
    CHECK(error.find("ssr") != string::npos);
}

TEST_CASE("graphics schema cook: a preset entry naming a missing option is a located error")
{
    json schema = SampleSchema();
    schema["Presets"][0]["Entries"][0]["OptionId"] = "medium"; // shadows has off/high only
    const string error = CookExpectingError("gfxschema_missingoption", schema);
    REQUIRE_FALSE(error.empty());
    CHECK(error.find("medium") != string::npos);
}

TEST_CASE(
    "graphics schema cook: a preset entry naming a non-render_scale built-in is a located error")
{
    json schema = SampleSchema();
    schema["Presets"][0]["Entries"].push_back({{"SettingId", "resolution"}, {"ScalarValue", 1.0}});
    const string error = CookExpectingError("gfxschema_badbuiltin", schema);
    REQUIRE_FALSE(error.empty());
    CHECK(error.find("resolution") != string::npos);
}
