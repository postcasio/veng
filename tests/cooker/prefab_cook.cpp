// Prefab cook test: cooks a *.prefab.json through the PrefabImporter against the
// reflected hello-triangle registry (loaded module: builtins + Spinner) and
// checks the CookedPrefabHeader, the entity/component tables, each component's
// TypeId, and that the record tail round-trips back through ReadFields to the
// authored values. Also covers each validation failure (unknown component, wrong
// field type, unknown field, reference range, AssetHandle type-match) and the
// no-module error.

#include <cstring>
#include <filesystem>
#include <fstream>

#include <doctest/doctest.h>

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Cook/ModuleTypes.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Entity.h>

using namespace Veng;
using namespace Veng::Cook;

namespace
{
    const path FixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
    constexpr TypeId SpinnerTypeId = 0xAEF00D5EFC2444DAULL;

    // Mirrors the hello-triangle Spinner's layout so ReadFields can decode by offset.
    struct SpinnerMirror
    {
        f32 SpeedRadiansPerSec = 0.0f;
        vec3 Axis = vec3(0.0f);
    };

    // Loads the reflected hello-triangle registry (builtins + Spinner). The
    // module image must outlive the registry, so the caller holds the returned
    // LoadedModuleTypes for the whole test.
    LoadedModuleTypes LoadRegistry()
    {
        Result<LoadedModuleTypes> loaded = LoadModuleTypes(path{VENG_HELLO_TRIANGLE_MODULE_PATH});
        REQUIRE(loaded.has_value());
        return std::move(*loaded);
    }

    // Cooks a prefab pack with the prefab importer + the reflected registry, and
    // returns the prefab blob bytes (or the located error).
    Result<vector<u8>> CookPrefab(const path& packJson, const TypeRegistry* types,
                                  std::span<const path> refs, AssetId prefabId)
    {
        Cooker cooker;
        RegisterBuiltinImporters(cooker);
        RegisterPrefabImporter(cooker);

        const path outArchive =
            std::filesystem::temp_directory_path() / "veng_cooker_prefab.vengpack";

        const VoidResult cookResult = cooker.CookPack(packJson, outArchive, refs, types);
        if (!cookResult.has_value())
        {
            return std::unexpected(cookResult.error());
        }

        const Result<ArchiveReader> reader = ArchiveReader::Open(outArchive);
        if (!reader.has_value())
        {
            return std::unexpected(reader.error());
        }

        const optional<ArchiveEntry> entry = reader->Find(prefabId);
        if (!entry.has_value())
        {
            return std::unexpected(string("prefab entry missing from archive"));
        }

        return vector<u8>(entry->Blob.begin(), entry->Blob.end());
    }

    // Writes a one-entity prefab JSON and a one-entry prefab pack into a temp dir,
    // returning the pack path. The prefab references no external assets.
    path WriteInlinePrefab(const string& name, const json& components)
    {
        const path dir = std::filesystem::temp_directory_path();
        const path prefabPath = dir / (name + ".prefab.json");
        const path packPath = dir / (name + ".pack.json");

        json prefab;
        prefab["entities"] = json::array();
        json entity;
        entity["name"] = "E";
        entity["components"] = components;
        prefab["entities"].push_back(entity);

        std::ofstream(prefabPath) << prefab.dump();

        json pack;
        pack["version"] = 1;
        pack["assets"] = json::array();
        json asset;
        asset["id"] = 4242;
        asset["type"] = "prefab";
        asset["source"] = prefabPath.filename().string();
        pack["assets"].push_back(asset);
        std::ofstream(packPath) << pack.dump();

        return packPath;
    }
}

TEST_CASE("prefab cook: happy path — header, component TypeIds, record round-trip")
{
    const LoadedModuleTypes module = LoadRegistry();
    const TypeRegistry& types = module.Types;

    const path packJson = FixtureDir / "prefab_pack.json";
    const path refPack = FixtureDir / "prefab_refs.json";
    const path refs[] = {refPack};

    const Result<vector<u8>> blobResult = CookPrefab(packJson, &types, refs, AssetId{9001});
    REQUIRE_MESSAGE(blobResult.has_value(),
                    "cook failed: ", blobResult ? string{} : blobResult.error());

    const vector<u8>& blob = *blobResult;
    REQUIRE(blob.size() >= sizeof(CookedPrefabHeader));

    CookedPrefabHeader header{};
    std::memcpy(&header, blob.data(), sizeof(header));

    CHECK(header.Version == CookedPrefabVersion);
    CHECK(header.EntityCount == 2);
    // Root: Name + Transform + MeshRenderer + Spinner (4); Child: Transform + Hierarchy (2).
    CHECK(header.ComponentCount == 6);

    const u8* cursor = blob.data() + sizeof(CookedPrefabHeader);

    const auto* entityTable = reinterpret_cast<const CookedPrefabEntity*>(cursor);
    cursor += header.EntityCount * sizeof(CookedPrefabEntity);

    const auto* componentTable = reinterpret_cast<const CookedPrefabComponent*>(cursor);
    cursor += header.ComponentCount * sizeof(CookedPrefabComponent);

    const u8* records = cursor;
    CHECK(static_cast<usize>((records - blob.data()) + header.RecordBytes) == blob.size());

    // Root has 4 components starting at 0; Child has 2 starting at 4.
    CHECK(entityTable[0].FirstComponent == 0);
    CHECK(entityTable[0].ComponentCount == 4);
    CHECK(entityTable[1].FirstComponent == 4);
    CHECK(entityTable[1].ComponentCount == 2);

    // Locate components by TypeId within Root's run.
    auto findComponent = [&](u32 first, u32 count, TypeId id) -> const CookedPrefabComponent*
    {
        for (u32 i = first; i < first + count; ++i)
        {
            if (componentTable[i].TypeId == id)
            {
                return &componentTable[i];
            }
        }
        return nullptr;
    };

    const CookedPrefabComponent* transform = findComponent(0, 4, TypeIdOf<Transform>());
    const CookedPrefabComponent* meshRenderer = findComponent(0, 4, TypeIdOf<MeshRenderer>());
    const CookedPrefabComponent* spinner = findComponent(0, 4, SpinnerTypeId);
    REQUIRE(transform != nullptr);
    REQUIRE(meshRenderer != nullptr);
    REQUIRE(spinner != nullptr);

    // --- Round-trip Transform via ReadFields ---
    Transform decodedTransform;
    ReadFields(std::span<const u8>(records + transform->RecordOffset, transform->RecordSize),
               &decodedTransform, types.Info(TypeIdOf<Transform>()), types);
    CHECK(decodedTransform.Position == vec3(1, 2, 3));
    // The quat JSON array is storage order [x,y,z,w]; [0,0,0,1] is the identity.
    CHECK(decodedTransform.Rotation == quat(1, 0, 0, 0)); // glm quat(w,x,y,z) — identity
    CHECK(decodedTransform.Scale == vec3(1, 1, 1));

    // --- Round-trip MeshRenderer's AssetHandle id (offset 0 = AssetId) ---
    MeshRenderer decodedRenderer;
    ReadFields(std::span<const u8>(records + meshRenderer->RecordOffset, meshRenderer->RecordSize),
               &decodedRenderer, types.Info(TypeIdOf<MeshRenderer>()), types);
    CHECK(decodedRenderer.Mesh.Id().Value == 8001ULL);

    // --- Round-trip Spinner via a mirror (the cooker has no compile-time type) ---
    SpinnerMirror decodedSpinner;
    ReadFields(std::span<const u8>(records + spinner->RecordOffset, spinner->RecordSize),
               &decodedSpinner, types.Info(SpinnerTypeId), types);
    CHECK(decodedSpinner.SpeedRadiansPerSec == doctest::Approx(1.5f));
    CHECK(decodedSpinner.Axis == vec3(0, 0, 1));

    // --- Child's Hierarchy parent edge cooks to {Index = 0 (prefab-local), Generation = 0} ---
    const CookedPrefabComponent* hierarchy = findComponent(
        entityTable[1].FirstComponent, entityTable[1].ComponentCount, TypeIdOf<Hierarchy>());
    REQUIRE(hierarchy != nullptr);
    Hierarchy decodedHierarchy;
    ReadFields(std::span<const u8>(records + hierarchy->RecordOffset, hierarchy->RecordSize),
               &decodedHierarchy, types.Info(TypeIdOf<Hierarchy>()), types);
    CHECK(decodedHierarchy.Parent.Index == 0u);
    CHECK(decodedHierarchy.Parent.Generation == 0u);
}

TEST_CASE("prefab cook: unknown component is a located error")
{
    const LoadedModuleTypes module = LoadRegistry();
    json components;
    components["NotARealComponent"] = json::object();
    const path packJson = WriteInlinePrefab("prefab_unknown_component", components);

    const Result<vector<u8>> blob = CookPrefab(packJson, &module.Types, {}, AssetId{4242});
    REQUIRE_FALSE(blob.has_value());
    CHECK(blob.error().find("unknown component") != string::npos);
    CHECK(blob.error().find("NotARealComponent") != string::npos);
}

TEST_CASE("prefab cook: a vec3 field given a scalar is a located error")
{
    const LoadedModuleTypes module = LoadRegistry();
    json transform;
    transform["Position"] = 3.0; // a scalar where a 3-array is required
    json components;
    components["::Veng::Transform"] = transform;
    const path packJson = WriteInlinePrefab("prefab_vec3_scalar", components);

    const Result<vector<u8>> blob = CookPrefab(packJson, &module.Types, {}, AssetId{4242});
    REQUIRE_FALSE(blob.has_value());
    CHECK(blob.error().find("Position") != string::npos);
    CHECK(blob.error().find("array of 3") != string::npos);
}

TEST_CASE("prefab cook: a string field given a number is a located error")
{
    const LoadedModuleTypes module = LoadRegistry();
    json name;
    name["Value"] = 42; // a number where a string is required
    json components;
    components["::Veng::Name"] = name;
    const path packJson = WriteInlinePrefab("prefab_string_number", components);

    const Result<vector<u8>> blob = CookPrefab(packJson, &module.Types, {}, AssetId{4242});
    REQUIRE_FALSE(blob.has_value());
    CHECK(blob.error().find("Value") != string::npos);
    CHECK(blob.error().find("string") != string::npos);
}

TEST_CASE("prefab cook: an unknown field is a located error")
{
    const LoadedModuleTypes module = LoadRegistry();
    json transform;
    transform["Nonexistent"] = 1.0;
    json components;
    components["::Veng::Transform"] = transform;
    const path packJson = WriteInlinePrefab("prefab_unknown_field", components);

    const Result<vector<u8>> blob = CookPrefab(packJson, &module.Types, {}, AssetId{4242});
    REQUIRE_FALSE(blob.has_value());
    CHECK(blob.error().find("Nonexistent") != string::npos);
    CHECK(blob.error().find("not in the component's descriptor") != string::npos);
}

TEST_CASE("prefab cook: an omitted field keeps its default value")
{
    const LoadedModuleTypes module = LoadRegistry();
    const TypeRegistry& types = module.Types;

    // A Transform with only Position set; Scale is omitted and must decode to its
    // default (1,1,1), not zero.
    json transform;
    transform["Position"] = json::array({5.0, 6.0, 7.0});
    json components;
    components["::Veng::Transform"] = transform;
    const path packJson = WriteInlinePrefab("prefab_omitted_field", components);

    const Result<vector<u8>> blobResult = CookPrefab(packJson, &types, {}, AssetId{4242});
    REQUIRE(blobResult.has_value());

    const vector<u8>& blob = *blobResult;
    CookedPrefabHeader header{};
    std::memcpy(&header, blob.data(), sizeof(header));
    REQUIRE(header.EntityCount == 1);
    REQUIRE(header.ComponentCount == 1);

    const auto* component = reinterpret_cast<const CookedPrefabComponent*>(
        blob.data() + sizeof(CookedPrefabHeader) + sizeof(CookedPrefabEntity));
    const u8* records = blob.data() + sizeof(CookedPrefabHeader) + sizeof(CookedPrefabEntity) +
                        sizeof(CookedPrefabComponent);

    Transform decoded;
    ReadFields(std::span<const u8>(records + component->RecordOffset, component->RecordSize),
               &decoded, types.Info(TypeIdOf<Transform>()), types);
    CHECK(decoded.Position == vec3(5, 6, 7));
    CHECK(decoded.Scale == vec3(1, 1, 1)); // the default survived
}

TEST_CASE("prefab cook: an out-of-range entity reference is a located error")
{
    const LoadedModuleTypes module = LoadRegistry();
    json hierarchy;
    hierarchy["Parent"] = 5; // only one entity in this prefab, so index 5 is out of range
    json components;
    components["::Veng::Hierarchy"] = hierarchy;
    const path packJson = WriteInlinePrefab("prefab_ref_oob", components);

    const Result<vector<u8>> blob = CookPrefab(packJson, &module.Types, {}, AssetId{4242});
    REQUIRE_FALSE(blob.has_value());
    CHECK(blob.error().find("out of range") != string::npos);
}

TEST_CASE("prefab cook: a null entity reference stays Entity::Null")
{
    const LoadedModuleTypes module = LoadRegistry();
    const TypeRegistry& types = module.Types;

    json hierarchy;
    hierarchy["Parent"] = nullptr;
    json components;
    components["::Veng::Hierarchy"] = hierarchy;
    const path packJson = WriteInlinePrefab("prefab_ref_null", components);

    const Result<vector<u8>> blobResult = CookPrefab(packJson, &types, {}, AssetId{4242});
    REQUIRE(blobResult.has_value());

    const vector<u8>& blob = *blobResult;
    const auto* component = reinterpret_cast<const CookedPrefabComponent*>(
        blob.data() + sizeof(CookedPrefabHeader) + sizeof(CookedPrefabEntity));
    const u8* records = blob.data() + sizeof(CookedPrefabHeader) + sizeof(CookedPrefabEntity) +
                        sizeof(CookedPrefabComponent);

    Hierarchy decoded;
    ReadFields(std::span<const u8>(records + component->RecordOffset, component->RecordSize),
               &decoded, types.Info(TypeIdOf<Hierarchy>()), types);
    CHECK(decoded.Parent == Entity::Null);
}

TEST_CASE("prefab cook: an AssetHandle id resolving to the wrong type is a located error")
{
    const LoadedModuleTypes module = LoadRegistry();

    // 8002 resolves to a Texture in the reference pack, but MeshRenderer.Mesh
    // expects a Mesh.
    json renderer;
    renderer["Mesh"] = 8002;
    json components;
    components["::Veng::MeshRenderer"] = renderer;

    const path dir = std::filesystem::temp_directory_path();
    const path prefabPath = dir / "prefab_handle_mismatch.prefab.json";
    json prefab;
    prefab["entities"] = json::array();
    json entity;
    entity["name"] = "E";
    entity["components"] = components;
    prefab["entities"].push_back(entity);
    std::ofstream(prefabPath) << prefab.dump();

    json pack;
    pack["version"] = 1;
    json asset;
    asset["id"] = 4242;
    asset["type"] = "prefab";
    asset["source"] = prefabPath.filename().string();
    pack["assets"] = json::array({asset});
    const path packPath = dir / "prefab_handle_mismatch.pack.json";
    std::ofstream(packPath) << pack.dump();

    const path refs[] = {FixtureDir / "prefab_refs.json"};
    const Result<vector<u8>> blob = CookPrefab(packPath, &module.Types, refs, AssetId{4242});
    REQUIRE_FALSE(blob.has_value());
    CHECK(blob.error().find("Mesh") != string::npos);
    CHECK(blob.error().find("expects type") != string::npos);
}

TEST_CASE("prefab cook: a non-resident AssetHandle id is accepted as-is")
{
    const LoadedModuleTypes module = LoadRegistry();

    // An id not in the pack or any reference pack: residency is the runtime's job.
    json renderer;
    renderer["Mesh"] = 1234567890123ULL;
    json components;
    components["::Veng::MeshRenderer"] = renderer;
    const path packJson = WriteInlinePrefab("prefab_handle_nonresident", components);

    const Result<vector<u8>> blob = CookPrefab(packJson, &module.Types, {}, AssetId{4242});
    REQUIRE_MESSAGE(blob.has_value(), "cook failed: ", blob ? string{} : blob.error());
}

TEST_CASE("prefab cook: cooking a prefab with no --module is the requires-module error")
{
    json components;
    components["::Veng::Transform"] = json::object();
    const path packJson = WriteInlinePrefab("prefab_no_module", components);

    // No registry passed.
    const Result<vector<u8>> blob = CookPrefab(packJson, nullptr, {}, AssetId{4242});
    REQUIRE_FALSE(blob.has_value());
    CHECK(blob.error().find("prefab cooking requires --module") != string::npos);
}
