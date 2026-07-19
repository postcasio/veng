// The whole builtin handle-field seam in one pass: a reflected component carrying
// AssetHandle<DataTable> and AssetHandle<TableSchema> is authored on a prefab, cooked (which runs
// the importer's type check through AssetTypeInfo::HandleFieldType), loaded through a real
// AssetManager (which runs the prefab loader's dependency collection through the same mapping),
// and spawned (which rehydrates the handles). Also covers the substitution rule
// AssetHandleFieldAccepts encodes as the cooker actually applies it — a MaterialInstance field
// given a bare Material id.
//
// Device-free: Renderer::Context is default-constructed and never initialized, and every loader
// on this path (prefab, table, schema) is CPU-only.

#include <filesystem>
#include <fstream>
#include <random>
#include <span>
#include "support/TempPath.h"

#include <doctest/doctest.h>
#include <fmt/format.h>

#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/DataTable.h>
#include <Veng/Asset/HexId.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Prefab.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Task/TaskSystem.h>

namespace VengTest
{
    /// @brief A component referencing table assets, the reference shape plan 03's picker needs.
    struct TableConsumer
    {
        /// @brief The table the component reads rows from.
        Veng::AssetHandle<Veng::DataTable> Table;
        /// @brief The schema those rows are laid out against.
        Veng::AssetHandle<Veng::TableSchema> Schema;
        /// @brief The row key the component looks up.
        Veng::i64 Key = 0;
    };

    /// @brief A component with a single material slot, for the field-accepts substitution rule.
    struct MaterialSlot
    {
        /// @brief The instance the slot draws with.
        Veng::AssetHandle<Veng::MaterialInstance> Material;
    };
}

VE_REFLECT(::VengTest::TableConsumer, 0xBCB25620151292C2ULL)
VE_FIELD(Table)
VE_FIELD(Schema)
VE_FIELD(Key)
VE_REFLECT_END();

VE_REFLECT(::VengTest::MaterialSlot, 0x572560894A64FBCFULL)
VE_FIELD(Material)
VE_REFLECT_END();

using namespace Veng;
using namespace Veng::Cook;

namespace
{
    constexpr u64 SchemaId = 0x11B1E5C0DE000001ULL;
    constexpr u64 TableId = 0x11B1E5C0DE000002ULL;
    constexpr u64 PrefabId = 0x11B1E5C0DE000003ULL;

    // Two integer-keyed rows over fixed-width columns, so the cook takes the fixed-stride path
    // and the runtime lookup below is a plain key search.
    json TuningSchema()
    {
        json schema;
        schema["columns"] = json::array({
            {{"name", "id"}, {"type", "Veng::i64"}},
            {{"name", "weight"}, {"type", "Veng::f32"}},
        });
        schema["key"] = "id";
        return schema;
    }

    json TuningTable()
    {
        json table;
        table["schema"] = FormatHexId(SchemaId);
        table["rows"] = json::array({
            {{"id", 7}, {"weight", 2.5}},
            {{"id", 3}, {"weight", -1.25}},
        });
        return table;
    }

    // Writes a schema + table + one-entity prefab and the pack naming all three, returning the
    // pack path. `consumer` is the TableConsumer component's authored JSON.
    path WritePack(const string& name, const json& consumer)
    {
        const path dir = Veng::TestSupport::TempDir();
        const path schemaPath = dir / (name + ".tableschema.json");
        const path tablePath = dir / (name + ".table.json");
        const path prefabPath = dir / (name + ".prefab.json");
        const path packPath = dir / (name + ".pack.json");

        std::ofstream(schemaPath) << TuningSchema().dump();
        std::ofstream(tablePath) << TuningTable().dump();

        json components;
        components["::VengTest::TableConsumer"] = consumer;
        json entity;
        entity["name"] = "Consumer";
        entity["components"] = components;
        json prefab;
        prefab["entities"] = json::array({entity});
        std::ofstream(prefabPath) << prefab.dump();

        json pack;
        pack["version"] = 1;
        pack["assets"] = json::array({
            {{"id", FormatHexId(SchemaId)},
             {"type", "TableSchema"},
             {"source", schemaPath.filename().string()}},
            {{"id", FormatHexId(TableId)},
             {"type", "DataTable"},
             {"source", tablePath.filename().string()}},
            {{"id", FormatHexId(PrefabId)},
             {"type", "Prefab"},
             {"source", prefabPath.filename().string()}},
        });
        std::ofstream(packPath) << pack.dump();

        return packPath;
    }

    // The reflected registry the cook and the runtime both use: engine builtins plus the
    // consumer component. One registry for both halves is what the launcher does too.
    void RegisterTypes(TypeRegistry& types)
    {
        RegisterBuiltinTypes(types);
        types.Register<VengTest::TableConsumer>();
        types.Register<VengTest::MaterialSlot>();
    }

    constexpr u64 MaterialRefId = 0x11B1E5C0DE000004ULL;
    constexpr u64 InstanceRefId = 0x11B1E5C0DE000005ULL;
    constexpr u64 TextureRefId = 0x11B1E5C0DE000006ULL;

    // A reference pack declaring one asset of each type the substitution cases need. Reference
    // packs are parsed for id → (source, type) only — nothing here is cooked — so the sources are
    // placeholders whose only job is to exist.
    path WriteMaterialRefs(const path& dir)
    {
        for (const string& name : {"ref.vmat.json", "ref.vmi.json", "ref.tex.json"})
        {
            std::ofstream(dir / name) << "{}";
        }

        json pack;
        pack["version"] = 1;
        pack["assets"] = json::array({
            {{"id", FormatHexId(MaterialRefId)}, {"type", "Material"}, {"source", "ref.vmat.json"}},
            {{"id", FormatHexId(InstanceRefId)},
             {"type", "MaterialInstance"},
             {"source", "ref.vmi.json"}},
            {{"id", FormatHexId(TextureRefId)}, {"type", "Texture"}, {"source", "ref.tex.json"}},
        });
        const path packPath = dir / "handle_leaf_refs.pack.json";
        std::ofstream(packPath) << pack.dump();
        return packPath;
    }

    // Cooks a one-entity prefab carrying `components` against `refs`, returning the located error
    // on failure. The prefab is the only cooked entry, so nothing needs a shader toolchain.
    Result<path> CookPrefabOnly(const string& name, const json& components,
                                std::span<const path> refs, const TypeRegistry& types)
    {
        const path dir = Veng::TestSupport::TempDir();
        const path prefabPath = dir / (name + ".prefab.json");
        const path packPath = dir / (name + ".pack.json");

        json entity;
        entity["name"] = "E";
        entity["components"] = components;
        json prefab;
        prefab["entities"] = json::array({entity});
        std::ofstream(prefabPath) << prefab.dump();

        json pack;
        pack["version"] = 1;
        pack["assets"] = json::array({{{"id", FormatHexId(PrefabId)},
                                       {"type", "Prefab"},
                                       {"source", prefabPath.filename().string()}}});
        std::ofstream(packPath) << pack.dump();

        Cooker cooker;
        RegisterBuiltinImporters(cooker);

        std::random_device rng;
        const path outArchive =
            Veng::TestSupport::TempDir() / fmt::format("veng_handle_accept_{:08x}.vengpack", rng());
        const VoidResult cooked = cooker.CookPack(packPath, outArchive, refs, &types);
        if (!cooked.has_value())
        {
            std::filesystem::remove(outArchive);
            return std::unexpected(cooked.error());
        }
        return outArchive;
    }

    // Cooks the pack into a temp archive, returning its path (the caller removes it) or the
    // located cook error.
    Result<path> CookPack(const path& packJson, const TypeRegistry& types)
    {
        Cooker cooker;
        RegisterBuiltinImporters(cooker);

        // Unique per call: ctest runs cases concurrently and a fixed name lets two cook over
        // each other's archive.
        std::random_device rng;
        const path outArchive =
            Veng::TestSupport::TempDir() / fmt::format("veng_handle_leaf_{:08x}.vengpack", rng());

        const VoidResult cooked = cooker.CookPack(packJson, outArchive, {}, &types);
        if (!cooked.has_value())
        {
            std::filesystem::remove(outArchive);
            return std::unexpected(cooked.error());
        }
        return outArchive;
    }
}

TEST_CASE("handle leaves: a DataTable reference cooks, loads, and rehydrates on spawn")
{
    TypeRegistry types;
    RegisterTypes(types);

    json consumer;
    consumer["Table"] = FormatHexId(TableId);
    consumer["Schema"] = FormatHexId(SchemaId);
    consumer["Key"] = 7;

    const Result<path> archive = CookPack(WritePack("handle_leaf_ok", consumer), types);
    REQUIRE_MESSAGE(archive.has_value(), "cook failed: ", archive ? string{} : archive.error());

    Renderer::Context context;
    TaskSystem tasks;
    AssetManager manager(context, tasks, types);
    REQUIRE(manager.Mount(*archive).has_value());

    // The prefab load is where the handle fields become dependencies: CollectHandleDeps turns
    // each field's leaf TypeId back into an asset type through the registry and loads it. Before
    // the leaves existed the fields could not be authored at all; without the registration this
    // load is an AssetError::Corrupt.
    const AssetResult<AssetHandle<Prefab>> prefab = manager.LoadSync<Prefab>(AssetId{PrefabId});
    REQUIRE_MESSAGE(prefab.has_value(),
                    "prefab load failed: ", prefab ? string{} : prefab.error().Detail);
    REQUIRE(prefab->IsLoaded());

    const Ref<Scene> scene = Scene::Create(types);
    const vector<Entity> roots = (*prefab)->SpawnInto(*scene, manager).Roots;
    REQUIRE(roots.size() == 1);

    const VengTest::TableConsumer* spawned = scene->TryGet<VengTest::TableConsumer>(roots[0]);
    REQUIRE(spawned != nullptr);

    // Rehydration gave back live handles, not bare ids — the whole point of a reference field.
    CHECK(spawned->Table.Id() == AssetId{TableId});
    CHECK(spawned->Schema.Id() == AssetId{SchemaId});
    REQUIRE(spawned->Table.IsLoaded());
    REQUIRE(spawned->Schema.IsLoaded());

    // The referenced table is usable through the handle, which is what a reference is for.
    const optional<u32> row = spawned->Table->FindRow(spawned->Key);
    REQUIRE(row.has_value());
    CHECK(spawned->Table->GetRowCount() == 2);

    std::filesystem::remove(*archive);
}

TEST_CASE("handle leaves: a DataTable field given a TableSchema id is a located cook error")
{
    TypeRegistry types;
    RegisterTypes(types);

    // The two table types are adjacent and easy to transpose, so this is the mismatch the cook
    // check most needs to catch. It only fires because both types now claim a handle leaf — an
    // unclaimed leaf is a cook error of its own, never a skipped check.
    json consumer;
    consumer["Table"] = FormatHexId(SchemaId);
    consumer["Schema"] = FormatHexId(SchemaId);

    const Result<path> archive = CookPack(WritePack("handle_leaf_swapped", consumer), types);
    REQUIRE_FALSE(archive.has_value());
    CHECK(archive.error().find("expects type") != string::npos);
    CHECK(archive.error().find("DataTable") != string::npos);
}

TEST_CASE("handle leaves: a TableSchema field given a DataTable id is a located cook error")
{
    TypeRegistry types;
    RegisterTypes(types);

    json consumer;
    consumer["Table"] = FormatHexId(TableId);
    consumer["Schema"] = FormatHexId(TableId);

    const Result<path> archive = CookPack(WritePack("handle_leaf_swapped_back", consumer), types);
    REQUIRE_FALSE(archive.has_value());
    CHECK(archive.error().find("expects type") != string::npos);
    CHECK(archive.error().find("TableSchema") != string::npos);
}

TEST_CASE("AssetHandleFieldAccepts: a MaterialInstance field cooks with a bare Material id")
{
    TypeRegistry types;
    RegisterTypes(types);

    const path refs[] = {WriteMaterialRefs(Veng::TestSupport::TempDir())};

    // The engine's one substitution, as the cooker applies it: the load resolves the material to
    // its zero-override default instance, so authoring the material id directly is legal.
    json components;
    components["::VengTest::MaterialSlot"] = {{"Material", FormatHexId(MaterialRefId)}};

    const Result<path> archive = CookPrefabOnly("handle_accept_material", components, refs, types);
    REQUIRE_MESSAGE(archive.has_value(), "cook failed: ", archive ? string{} : archive.error());
    std::filesystem::remove(*archive);
}

TEST_CASE("AssetHandleFieldAccepts: the field's own type cooks, an unrelated one does not")
{
    TypeRegistry types;
    RegisterTypes(types);

    const path refs[] = {WriteMaterialRefs(Veng::TestSupport::TempDir())};

    json exact;
    exact["::VengTest::MaterialSlot"] = {{"Material", FormatHexId(InstanceRefId)}};
    const Result<path> exactArchive = CookPrefabOnly("handle_accept_exact", exact, refs, types);
    REQUIRE_MESSAGE(exactArchive.has_value(),
                    "cook failed: ", exactArchive ? string{} : exactArchive.error());
    std::filesystem::remove(*exactArchive);

    // The substitution is one specific pair, not a general leniency — a Texture is still a
    // located error in the same field.
    json wrong;
    wrong["::VengTest::MaterialSlot"] = {{"Material", FormatHexId(TextureRefId)}};
    const Result<path> wrongArchive = CookPrefabOnly("handle_accept_wrong", wrong, refs, types);
    REQUIRE_FALSE(wrongArchive.has_value());
    CHECK(wrongArchive.error().find("expects type") != string::npos);
    CHECK(wrongArchive.error().find("MaterialInstance") != string::npos);
}
