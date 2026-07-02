// Input-map cook test: cooks a *.inputmap.json through the InputMapImporter and checks the
// CookedInputMapHeader plus that the { actions, bindings } record round-trips back through
// ReadFields into the resolver-ready form InputMappingContext exposes. Also covers each
// validation failure — an unknown-action binding, a Button/axis kind mismatch, a null id, a
// duplicate id, an unknown enum name. An input map needs no --module (it references only engine
// builtins), so the cook runs with a builtin-only registry and no module load.

#include <cstring>
#include <filesystem>
#include <fstream>

#include <doctest/doctest.h>

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/CookedBlobs.h>
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

        const path outArchive =
            std::filesystem::temp_directory_path() / "veng_cooker_inputmap.vengpack";

        const VoidResult cookResult = cooker.CookPack(packJson, outArchive);
        if (!cookResult.has_value())
        {
            return std::unexpected(cookResult.error());
        }

        const Result<ArchiveReader> reader = ArchiveReader::Open(outArchive);
        if (!reader.has_value())
        {
            return std::unexpected(reader.error());
        }

        const optional<ArchiveEntry> entry = reader->Find(mapId);
        if (!entry.has_value())
        {
            return std::unexpected(string("input map entry missing from archive"));
        }
        return vector<u8>(entry->Blob.begin(), entry->Blob.end());
    }

    // Writes an inputmap JSON + a one-entry pack into a temp dir, returning the pack path.
    path WriteInputMapPack(const string& name, const json& map)
    {
        const path dir = std::filesystem::temp_directory_path();
        const path mapPath = dir / (name + ".inputmap.json");
        const path packPath = dir / (name + ".pack.json");

        std::ofstream(mapPath) << map.dump();

        json pack;
        pack["version"] = 1;
        json asset;
        asset["id"] = 7777;
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
        map["actions"] = json::array({{{"id", MoveId}, {"name", "Move"}, {"kind", "Axis2D"}},
                                      {{"id", JumpId}, {"name", "Jump"}, {"kind", "Button"}}});
        map["bindings"] = json::array({{{"source", {{"device", "Keyboard"}, {"control", 68}}},
                                        {"action", MoveId},
                                        {"axis", "X"},
                                        {"scale", 1.0}},
                                       {{"source", {{"device", "Keyboard"}, {"control", 65}}},
                                        {"action", MoveId},
                                        {"axis", "X"},
                                        {"scale", -1.0}},
                                       {{"source", {{"device", "Keyboard"}, {"control", 87}}},
                                        {"action", MoveId},
                                        {"axis", "Y"},
                                        {"scale", 1.0}},
                                       {{"source", {{"device", "Keyboard"}, {"control", 32}}},
                                        {"action", JumpId},
                                        {"axis", "Whole"}}});
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

TEST_CASE("input map cook: a binding onto an undeclared action is a located error")
{
    json map = SampleMap();
    // Bind a control to an action id this context never declares.
    map["bindings"].push_back({{"source", {{"device", "Keyboard"}, {"control", 70}}},
                               {"action", 0x1234567890ABCDEFULL},
                               {"axis", "Whole"}});
    const path packJson = WriteInputMapPack("inputmap_unknown_action", map);

    const Result<vector<u8>> blob = CookInputMap(packJson, AssetId{7777});
    REQUIRE_FALSE(blob.has_value());
    CHECK(blob.error().find("not declared") != string::npos);
}

TEST_CASE("input map cook: an X/Y component on a Button action is a located error")
{
    json map = SampleMap();
    // Jump is a Button; an X component onto it is a kind/axis mismatch.
    map["bindings"].push_back(
        {{"source", {{"device", "Keyboard"}, {"control", 71}}}, {"action", JumpId}, {"axis", "X"}});
    const path packJson = WriteInputMapPack("inputmap_axis_mismatch", map);

    const Result<vector<u8>> blob = CookInputMap(packJson, AssetId{7777});
    REQUIRE_FALSE(blob.has_value());
    CHECK(blob.error().find("Button action") != string::npos);
}

TEST_CASE("input map cook: a null action id is a located error")
{
    json map = SampleMap();
    map["actions"].push_back({{"id", 0}, {"name", "Bad"}, {"kind", "Button"}});
    const path packJson = WriteInputMapPack("inputmap_null_id", map);

    const Result<vector<u8>> blob = CookInputMap(packJson, AssetId{7777});
    REQUIRE_FALSE(blob.has_value());
    CHECK(blob.error().find("non-null") != string::npos);
}

TEST_CASE("input map cook: a duplicate action id is a located error")
{
    json map = SampleMap();
    map["actions"].push_back({{"id", MoveId}, {"name", "MoveAgain"}, {"kind", "Axis2D"}});
    const path packJson = WriteInputMapPack("inputmap_dup_id", map);

    const Result<vector<u8>> blob = CookInputMap(packJson, AssetId{7777});
    REQUIRE_FALSE(blob.has_value());
    CHECK(blob.error().find("more than once") != string::npos);
}

TEST_CASE("input map cook: an unknown enum name is a located error")
{
    json map = SampleMap();
    map["actions"][0]["kind"] = "NotAKind";
    const path packJson = WriteInputMapPack("inputmap_bad_enum", map);

    const Result<vector<u8>> blob = CookInputMap(packJson, AssetId{7777});
    REQUIRE_FALSE(blob.has_value());
    CHECK(blob.error().find("unknown value") != string::npos);
}
