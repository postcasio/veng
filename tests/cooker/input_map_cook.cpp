// Input-map cook test: cooks a *.inputmap.json through the InputMapImporter and checks the
// CookedInputMapHeader plus that the { actions, bindings } record round-trips back through
// ReadFields into the resolver-ready form InputMappingContext exposes. Also covers each
// validation failure — an unknown-action binding, a Button/axis kind mismatch, a null id, a
// duplicate id, an unknown enum name, an unknown key, out-of-range shaping, and an unreadable
// source or chord modifier. An input map needs no --module (it references only engine builtins), so
// the cook runs with a builtin-only registry and no module load.

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
#include <Veng/Asset/InputMappingContext.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Input/Actions.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>

using namespace Veng;
using namespace Veng::Cook;

namespace
{
    constexpr u64 MoveId = 8360947520741195460ULL;
    constexpr u64 JumpId = 13135361833009734947ULL;

    // Cooks a one-entry input-map pack and returns the cooked blob bytes (or the located error).
    Result<vector<u8>> CookInputMap(const path& packJson, AssetId mapId)
    {
        Cooker cooker;
        RegisterBuiltinImporters(cooker);

        // Unique per call: ctest runs each case as its own process in parallel, and a
        // shared fixed name lets concurrent cases cook over and delete each other's archive.
        std::random_device rng;
        const path outArchive = Veng::TestSupport::TempDir() /
                                fmt::format("veng_cooker_inputmap_{:08x}.vengpack", rng());

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

        const optional<ArchiveEntry> entry = reader->Find(mapId);
        if (!entry.has_value())
        {
            std::filesystem::remove(outArchive);
            return std::unexpected(string("input map entry missing from archive"));
        }
        vector<u8> blob(entry->Blob.begin(), entry->Blob.end());
        std::filesystem::remove(outArchive);
        return blob;
    }

    // Writes an inputmap JSON + a one-entry pack into a temp dir, returning the pack path.
    path WriteInputMapPack(const string& name, const json& map)
    {
        const path dir = Veng::TestSupport::TempDir();
        const path mapPath = dir / (name + ".inputmap.json");
        const path packPath = dir / (name + ".pack.json");

        std::ofstream(mapPath) << map.dump();

        json pack;
        pack["version"] = 1;
        json asset;
        asset["id"] = FormatHexId(7777);
        asset["type"] = "InputMap";
        asset["source"] = mapPath.filename().string();
        pack["assets"] = json::array({asset});
        std::ofstream(packPath) << pack.dump();

        return packPath;
    }

    // A well-formed sample: a 2D Move action bound to WASD and a Jump button bound to Space.
    json SampleMap()
    {
        json map;
        const string moveId = FormatHexId(MoveId);
        const string jumpId = FormatHexId(JumpId);
        map["Actions"] = json::array({{{"Id", moveId}, {"Name", "Move"}, {"Kind", "Axis2D"}},
                                      {{"Id", jumpId}, {"Name", "Jump"}, {"Kind", "Button"}}});
        map["Bindings"] = json::array({{{"Source", {{"Device", "Keyboard"}, {"Control", 68}}},
                                        {"Action", moveId},
                                        {"Axis", "X"},
                                        {"Scale", 1.0}},
                                       {{"Source", {{"Device", "Keyboard"}, {"Control", 65}}},
                                        {"Action", moveId},
                                        {"Axis", "X"},
                                        {"Scale", -1.0}},
                                       {{"Source", {{"Device", "Keyboard"}, {"Control", 87}}},
                                        {"Action", moveId},
                                        {"Axis", "Y"},
                                        {"Scale", 1.0}},
                                       {{"Source", {{"Device", "Keyboard"}, {"Control", 32}}},
                                        {"Action", jumpId},
                                        {"Axis", "Whole"}}});
        return map;
    }
}

TEST_CASE("input map cook: happy path — header + resolved context round-trip")
{
    const path packJson = WriteInputMapPack("inputmap_happy", SampleMap());

    const Result<vector<u8>> blobResult = CookInputMap(packJson, AssetId{7777});
    REQUIRE_MESSAGE(blobResult.has_value(),
                    "cook failed: ", blobResult ? string{} : blobResult.error());

    const vector<u8>& blob = *blobResult;
    REQUIRE(blob.size() >= sizeof(CookedInputMapHeader));

    CookedInputMapHeader header{};
    std::memcpy(&header, blob.data(), sizeof(header));
    CHECK(header.Version == CookedInputMapVersion);
    REQUIRE(blob.size() == sizeof(CookedInputMapHeader) + header.RecordBytes);

    // Decode the record the way the runtime loader does, then build the context and check the
    // resolver-ready form matches the source.
    TypeRegistry registry;
    RegisterBuiltinTypes(registry);

    const std::span<const u8> record(blob.data() + sizeof(CookedInputMapHeader),
                                     header.RecordBytes);
    InputMapData data;
    REQUIRE(
        ReadFields(record, &data, registry.Info(TypeIdOf<InputMapData>()), registry).has_value());

    const Ref<InputMappingContext> context =
        InputMappingContext::Create(std::move(data.Actions), std::move(data.Bindings));
    const ResolvedContext& resolved = context->GetResolved();

    REQUIRE(resolved.Actions.size() == 2);
    CHECK(static_cast<u64>(resolved.Actions[0].Id) == MoveId);
    CHECK(resolved.Actions[0].Name == "Move");
    CHECK(resolved.Actions[0].Kind == ActionKind::Axis2D);
    CHECK(static_cast<u64>(resolved.Actions[1].Id) == JumpId);
    CHECK(resolved.Actions[1].Kind == ActionKind::Button);

    REQUIRE(resolved.Bindings.size() == 4);
    CHECK(resolved.Bindings[0].Source.Device == InputDeviceType::Keyboard);
    CHECK(resolved.Bindings[0].Source.Control == 68u);
    CHECK(static_cast<u64>(resolved.Bindings[0].Action) == MoveId);
    CHECK(resolved.Bindings[0].Axis == AxisComponent::X);
    CHECK(resolved.Bindings[0].Scale == doctest::Approx(1.0f));
    CHECK(resolved.Bindings[1].Scale == doctest::Approx(-1.0f));
    CHECK(resolved.Bindings[3].Axis == AxisComponent::Whole);
}

TEST_CASE("input map cook: requiresGameplayFocus authors into the resolved context")
{
    json map = SampleMap();
    map["RequiresGameplayFocus"] = true;
    const path packJson = WriteInputMapPack("inputmap_focus", map);

    const Result<vector<u8>> blobResult = CookInputMap(packJson, AssetId{7777});
    REQUIRE_MESSAGE(blobResult.has_value(),
                    "cook failed: ", blobResult ? string{} : blobResult.error());
    const vector<u8>& blob = *blobResult;

    TypeRegistry registry;
    RegisterBuiltinTypes(registry);
    CookedInputMapHeader header{};
    std::memcpy(&header, blob.data(), sizeof(header));
    const std::span<const u8> record(blob.data() + sizeof(CookedInputMapHeader),
                                     header.RecordBytes);

    InputMapData data;
    REQUIRE(
        ReadFields(record, &data, registry.Info(TypeIdOf<InputMapData>()), registry).has_value());
    CHECK(data.RequiresGameplayFocus);

    // The gate carries into the resolver-ready form the InputMappingSystem reads.
    const Ref<InputMappingContext> context = InputMappingContext::Create(
        std::move(data.Actions), std::move(data.Bindings), data.RequiresGameplayFocus);
    CHECK(context->GetResolved().RequiresGameplayFocus);
}

TEST_CASE("input map cook: a map not authoring requiresGameplayFocus defaults to false")
{
    // SampleMap authors no gate; the field defaults false, so an existing map is unchanged.
    const path packJson = WriteInputMapPack("inputmap_nofocus", SampleMap());
    const Result<vector<u8>> blobResult = CookInputMap(packJson, AssetId{7777});
    REQUIRE(blobResult.has_value());

    TypeRegistry registry;
    RegisterBuiltinTypes(registry);
    CookedInputMapHeader header{};
    std::memcpy(&header, blobResult->data(), sizeof(header));
    const std::span<const u8> record(blobResult->data() + sizeof(CookedInputMapHeader),
                                     header.RecordBytes);
    InputMapData data;
    REQUIRE(
        ReadFields(record, &data, registry.Info(TypeIdOf<InputMapData>()), registry).has_value());
    CHECK_FALSE(data.RequiresGameplayFocus);
}

TEST_CASE("input map load: a pre-change blob without the gate field tolerant-reads to false")
{
    TypeRegistry registry;
    RegisterBuiltinTypes(registry);
    const TypeInfo& full = registry.Info(TypeIdOf<InputMapData>());

    // Encode through a TypeInfo carrying only the original { Actions, Bindings } descriptors, so the
    // record names no RequiresGameplayFocus — exactly a blob cooked before the field existed.
    TypeInfo older = full;
    std::erase_if(older.Fields,
                  [](const FieldDescriptor& f) { return f.Name == "RequiresGameplayFocus"; });
    REQUIRE(older.Fields.size() == full.Fields.size() - 1);

    InputMapData authored;
    authored.Actions.push_back(InputAction{
        .Id = static_cast<ActionId>(MoveId), .Name = "Move", .Kind = ActionKind::Axis2D});
    authored.RequiresGameplayFocus = true; // dropped by the old encoder

    vector<u8> record;
    WriteFields(record, &authored, older, registry);

    // The current loader reads it through the full 3-field TypeInfo; the missing field stays at its
    // default (false), so an existing cooked map loads unchanged within CookedInputMapVersion.
    InputMapData loaded;
    REQUIRE(ReadFields(record, &loaded, full, registry).has_value());
    REQUIRE(loaded.Actions.size() == 1);
    CHECK_FALSE(loaded.RequiresGameplayFocus);
}

TEST_CASE("input map cook: a binding onto an undeclared action is a located error")
{
    json map = SampleMap();
    // Bind a control to an action id this context never declares.
    map["Bindings"].push_back({{"Source", {{"Device", "Keyboard"}, {"Control", 70}}},
                               {"Action", FormatHexId(0x1234567890ABCDEFULL)},
                               {"Axis", "Whole"}});
    const path packJson = WriteInputMapPack("inputmap_unknown_action", map);

    const Result<vector<u8>> blob = CookInputMap(packJson, AssetId{7777});
    REQUIRE_FALSE(blob.has_value());
    CHECK(blob.error().find("not declared") != string::npos);
}

TEST_CASE("input map cook: an X/Y component on a Button action is a located error")
{
    json map = SampleMap();
    // Jump is a Button; an X component onto it is a kind/axis mismatch.
    map["Bindings"].push_back({{"Source", {{"Device", "Keyboard"}, {"Control", 71}}},
                               {"Action", FormatHexId(JumpId)},
                               {"Axis", "X"}});
    const path packJson = WriteInputMapPack("inputmap_axis_mismatch", map);

    const Result<vector<u8>> blob = CookInputMap(packJson, AssetId{7777});
    REQUIRE_FALSE(blob.has_value());
    CHECK(blob.error().find("Button action") != string::npos);
}

TEST_CASE("input map cook: a null action id is a located error")
{
    json map = SampleMap();
    map["Actions"].push_back({{"Id", FormatHexId(0)}, {"Name", "Bad"}, {"Kind", "Button"}});
    const path packJson = WriteInputMapPack("inputmap_null_id", map);

    const Result<vector<u8>> blob = CookInputMap(packJson, AssetId{7777});
    REQUIRE_FALSE(blob.has_value());
    CHECK(blob.error().find("non-null") != string::npos);
}

TEST_CASE("input map cook: a duplicate action id is a located error")
{
    json map = SampleMap();
    map["Actions"].push_back(
        {{"Id", FormatHexId(MoveId)}, {"Name", "MoveAgain"}, {"Kind", "Axis2D"}});
    const path packJson = WriteInputMapPack("inputmap_dup_id", map);

    const Result<vector<u8>> blob = CookInputMap(packJson, AssetId{7777});
    REQUIRE_FALSE(blob.has_value());
    CHECK(blob.error().find("more than once") != string::npos);
}

TEST_CASE("input map cook: an unknown enum name is a located error")
{
    json map = SampleMap();
    map["Actions"][0]["Kind"] = "NotAKind";
    const path packJson = WriteInputMapPack("inputmap_bad_enum", map);

    const Result<vector<u8>> blob = CookInputMap(packJson, AssetId{7777});
    REQUIRE_FALSE(blob.has_value());
    CHECK(blob.error().find("unknown enumerator") != string::npos);
}

TEST_CASE("input map cook: a binding's threshold and exponent author into the resolved context")
{
    json map = SampleMap();
    map["Bindings"][0]["Threshold"] = 0.25;
    map["Bindings"][0]["Exponent"] = 2.0;
    const path packJson = WriteInputMapPack("inputmap_shaping", map);

    const Result<vector<u8>> blobResult = CookInputMap(packJson, AssetId{7777});
    REQUIRE_MESSAGE(blobResult.has_value(),
                    "cook failed: ", blobResult ? string{} : blobResult.error());

    TypeRegistry registry;
    RegisterBuiltinTypes(registry);
    CookedInputMapHeader header{};
    std::memcpy(&header, blobResult->data(), sizeof(header));
    const std::span<const u8> record(blobResult->data() + sizeof(CookedInputMapHeader),
                                     header.RecordBytes);
    InputMapData data;
    REQUIRE(
        ReadFields(record, &data, registry.Info(TypeIdOf<InputMapData>()), registry).has_value());
    REQUIRE(data.Bindings.size() == 4);
    CHECK(data.Bindings[0].Threshold == doctest::Approx(0.25f));
    CHECK(data.Bindings[0].Exponent == doctest::Approx(2.0f));
    // A binding authoring neither keeps the defaults that leave its value unchanged.
    CHECK(data.Bindings[1].Threshold == 0.0f);
    CHECK(data.Bindings[1].Exponent == 1.0f);
}

TEST_CASE("input map cook: a negative threshold or a non-positive exponent is a located error")
{
    json negative = SampleMap();
    negative["Bindings"][0]["Threshold"] = -0.1;
    const Result<vector<u8>> negativeBlob =
        CookInputMap(WriteInputMapPack("inputmap_bad_threshold", negative), AssetId{7777});
    REQUIRE_FALSE(negativeBlob.has_value());
    CHECK(negativeBlob.error().find("Threshold") != string::npos);

    json flat = SampleMap();
    flat["Bindings"][0]["Exponent"] = 0.0;
    const Result<vector<u8>> flatBlob =
        CookInputMap(WriteInputMapPack("inputmap_bad_exponent", flat), AssetId{7777});
    REQUIRE_FALSE(flatBlob.has_value());
    CHECK(flatBlob.error().find("Exponent") != string::npos);
}

TEST_CASE("input map cook: a chord's modifier and its threshold author into the resolved context")
{
    json map = SampleMap();
    map["Bindings"][0]["Modifier"] = {{"Device", "GamepadAxis"}, {"Control", 4}};
    map["Bindings"][0]["ModifierThreshold"] = 0.75;
    const path packJson = WriteInputMapPack("inputmap_chord", map);

    const Result<vector<u8>> blobResult = CookInputMap(packJson, AssetId{7777});
    REQUIRE_MESSAGE(blobResult.has_value(),
                    "cook failed: ", blobResult ? string{} : blobResult.error());

    TypeRegistry registry;
    RegisterBuiltinTypes(registry);
    CookedInputMapHeader header{};
    std::memcpy(&header, blobResult->data(), sizeof(header));
    const std::span<const u8> record(blobResult->data() + sizeof(CookedInputMapHeader),
                                     header.RecordBytes);
    InputMapData data;
    REQUIRE(
        ReadFields(record, &data, registry.Info(TypeIdOf<InputMapData>()), registry).has_value());
    REQUIRE(data.Bindings.size() == 4);
    CHECK(data.Bindings[0].Modifier.Device == InputDeviceType::GamepadAxis);
    CHECK(data.Bindings[0].Modifier.Control == 4u);
    CHECK(data.Bindings[0].ModifierThreshold == doctest::Approx(0.75f));
    // A binding authoring no modifier stays plain.
    CHECK(data.Bindings[1].Modifier.Device == InputDeviceType::None);
    CHECK(data.Bindings[1].ModifierThreshold == doctest::Approx(0.5f));
}

TEST_CASE("input map cook: an unreadable source or modifier is a located error")
{
    const auto cookError = [](const string& name, const json& map)
    {
        const Result<vector<u8>> blob = CookInputMap(WriteInputMapPack(name, map), AssetId{7777});
        REQUIRE_FALSE(blob.has_value());
        return blob.error();
    };

    SUBCASE("a source with no device")
    {
        json map = SampleMap();
        map["Bindings"][0]["Source"]["Device"] = "None";
        CHECK(cookError("inputmap_source_none", map).find("'None'") != string::npos);
    }

    SUBCASE("a modifier past the last pad button")
    {
        json map = SampleMap();
        map["Bindings"][0]["Modifier"] = {{"Device", "GamepadButton"}, {"Control", 999}};
        CHECK(cookError("inputmap_modifier_range", map).find("'Modifier'") != string::npos);
    }

    SUBCASE("a negative modifier threshold")
    {
        json map = SampleMap();
        map["Bindings"][0]["Modifier"] = {{"Device", "GamepadAxis"}, {"Control", 4}};
        map["Bindings"][0]["ModifierThreshold"] = -0.5;
        CHECK(cookError("inputmap_modifier_negative", map).find("ModifierThreshold") !=
              string::npos);
    }

    SUBCASE("a digital modifier that could never reach its threshold")
    {
        json map = SampleMap();
        map["Bindings"][0]["Modifier"] = {{"Device", "Keyboard"}, {"Control", 340}};
        map["Bindings"][0]["ModifierThreshold"] = 1.5;
        CHECK(cookError("inputmap_modifier_unreachable", map).find("never down") != string::npos);
    }
}

TEST_CASE("input map cook: a key naming no reflected field is a located error")
{
    json map = SampleMap();
    map["Bindings"][0]["scale"] = 2.0;
    const Result<vector<u8>> blob =
        CookInputMap(WriteInputMapPack("inputmap_unknown_key", map), AssetId{7777});
    REQUIRE_FALSE(blob.has_value());
    CHECK(blob.error().find("unknown field") != string::npos);
}
