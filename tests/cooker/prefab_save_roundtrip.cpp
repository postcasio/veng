// Prefab save-back round-trip test: builds a live Scene, writes it to a .prefab.json
// through the editor's reflection-driven writer (PrefabSerialize::Save), then cooks
// that source through the real PrefabImporter and asserts the cooked records decode
// back to the authored values — pinning the writer↔cooker symmetry arm by arm. The
// fixture exercises every FieldClass the cook reads (scalar / vector / quaternion /
// string / asset-handle / enum / reference / variant), plus the structural cases a
// naive patch breaks: an injected unknown key survives a save, and a delete + reorder
// re-aligns each surviving entity to its source object by the stable per-entity id.
// Pure CPU — no Context, no Vulkan symbol touched (cooker / unit band, no ICD).

#include <cstring>
#include <filesystem>
#include <fstream>
#include <sstream>

#include <doctest/doctest.h>

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Scene/Scene.h>

#include <nlohmann/json.hpp>

#include "PrefabSerialize.h"

using namespace Veng;
using namespace Veng::Cook;

// ---- A component exercising every cook-readable FieldClass -------------------

namespace
{
    // The variant whose alternatives the writer/reader recurse into.
    struct Sphere
    {
        f32 Radius = 0.0f;
        AssetHandle<Material> Material;
    };

    struct Box
    {
        f32 Width = 0.0f;
    };

    using ShapeVariant = Variant<Sphere, Box>;

    // A component with a dynamic-array field, for the writer's Array arm. Prefab sources do not
    // cook array fields (BindField rejects them), so this is exercised writer-only, not cooked.
    struct ArrayHolder
    {
        vector<f32> Values;
    };

    struct AllFields
    {
        bool Flag = false;
        f32 Real = 0.0f;
        i32 Signed = 0;
        u32 Unsigned = 0;
        u64 Big = 0;
        vec3 Vector{0.0f};
        quat Quaternion{1.0f, 0.0f, 0.0f, 0.0f};
        string Text;
        AssetHandle<Mesh> Mesh;
        LightType Enum = LightType::Directional;
        ShapeVariant Shape;
    };
}

VE_REFLECT(::Sphere, 0x51A2C3D4E5F60718ULL)
VE_FIELD(Radius)
VE_FIELD(Material)
VE_REFLECT_END();

VE_REFLECT(::Box, 0x6B2C3D4E5F607182ULL)
VE_FIELD(Width)
VE_REFLECT_END();

VE_VARIANT(::ShapeVariant, 0x7C3D4E5F60718293ULL);

VE_REFLECT(::ArrayHolder, 0x9E5F607182930415ULL)
VE_ARRAY_FIELD(Values)
VE_REFLECT_END();

VE_REFLECT(::AllFields, 0x8D4E5F6071829304ULL)
VE_FIELD(Flag)
VE_FIELD(Real)
VE_FIELD(Signed)
VE_FIELD(Unsigned)
VE_FIELD(Big)
VE_FIELD(Vector)
VE_FIELD(Quaternion)
VE_FIELD(Text)
VE_FIELD(Mesh)
VE_FIELD(Enum)
VE_FIELD(Shape)
VE_REFLECT_END();

namespace
{
    using json = nlohmann::json;

    TypeRegistry BuildRegistry()
    {
        TypeRegistry registry;
        RegisterBuiltinTypes(registry);
        registry.Register<AllFields>();
        registry.Register<ShapeVariant>();
        registry.Register<ArrayHolder>();
        return registry;
    }

    // Writes a one-asset prefab pack pointing at `prefabPath`, returning the pack path.
    path WritePack(const string& name, const path& prefabPath, AssetId id)
    {
        const path packPath = std::filesystem::temp_directory_path() / (name + ".pack.json");
        json pack;
        pack["version"] = 1;
        json asset;
        asset["id"] = id.Value;
        asset["type"] = "prefab";
        asset["source"] = prefabPath.filename().string();
        pack["assets"] = json::array({asset});
        std::ofstream(packPath) << pack.dump();
        return packPath;
    }

    // Cooks the prefab at `prefabPath` (id `id`) against `registry`, returning the cooked blob.
    Result<vector<u8>> CookPrefabSource(const string& name, const path& prefabPath, AssetId id,
                                        const TypeRegistry& registry)
    {
        const path packPath = WritePack(name, prefabPath, id);

        Cooker cooker;
        RegisterPrefabImporter(cooker);

        const path outArchive = std::filesystem::temp_directory_path() / (name + ".vengpack");
        const VoidResult cooked = cooker.CookPack(packPath, outArchive, {}, &registry);
        if (!cooked.has_value())
        {
            return std::unexpected(cooked.error());
        }

        const Result<ArchiveReader> reader = ArchiveReader::Open(outArchive);
        if (!reader.has_value())
        {
            return std::unexpected(reader.error());
        }
        const optional<ArchiveEntry> archived = reader->Find(id);
        if (!archived.has_value())
        {
            return std::unexpected(string("prefab entry missing from archive"));
        }
        return vector<u8>(archived->Blob.begin(), archived->Blob.end());
    }

    // Reads the parsed JSON document a save produced.
    json ReadDoc(const path& file)
    {
        const std::ifstream in(file, std::ios::binary);
        std::ostringstream contents;
        contents << in.rdbuf();
        return json::parse(contents.str(), nullptr, false);
    }

    // Slices entity `e`'s component `id` record out of a cooked prefab blob, or empty if absent.
    vector<u8> ComponentRecord(const vector<u8>& blob, u32 entityIndex, TypeId id)
    {
        CookedPrefabHeader header{};
        std::memcpy(&header, blob.data(), sizeof(header));
        const auto* entities =
            reinterpret_cast<const CookedPrefabEntity*>(blob.data() + sizeof(header));
        const auto* components = reinterpret_cast<const CookedPrefabComponent*>(
            blob.data() + sizeof(header) + header.EntityCount * sizeof(CookedPrefabEntity));
        const u8* records = blob.data() + sizeof(header) +
                            header.EntityCount * sizeof(CookedPrefabEntity) +
                            header.ComponentCount * sizeof(CookedPrefabComponent);

        const CookedPrefabEntity& entity = entities[entityIndex];
        for (u32 i = entity.FirstComponent; i < entity.FirstComponent + entity.ComponentCount; ++i)
        {
            if (components[i].TypeId == id)
            {
                return vector<u8>(records + components[i].RecordOffset,
                                  records + components[i].RecordOffset + components[i].RecordSize);
            }
        }
        return {};
    }
}

TEST_CASE("prefab save: every FieldClass round-trips through save → cook → decode")
{
    const TypeRegistry registry = BuildRegistry();

    // A root entity carrying every cook-readable FieldClass, and a child referencing it.
    const auto scene = Scene::Create(const_cast<TypeRegistry&>(registry));
    const Entity root = scene->CreateEntity();
    auto& all = scene->Add<AllFields>(root);
    all.Flag = true;
    all.Real = 1.5f;
    all.Signed = -7;
    all.Unsigned = 42u;
    all.Big = 0xABCDEF0123456789ULL;
    all.Vector = vec3(1.0f, 2.0f, 3.0f);
    all.Quaternion = quat(0.7071f, 0.0f, 0.7071f, 0.0f); // glm quat(w,x,y,z)
    all.Text = "hello";
    all.Mesh = AssetHandle<Mesh>{}; // id-less is fine; the writer emits its u64 id (0)
    all.Enum = LightType::Spot;
    *static_cast<Sphere*>(all.Shape.SetActive(TypeIdOf<Sphere>())) = Sphere{.Radius = 0.5f};

    const Entity child = scene->CreateEntity();
    scene->Add<Name>(child) = Name{.Value = "Child"};
    scene->SetParent(child, root); // a Hierarchy.Parent Reference back to the root

    const path prefabPath = std::filesystem::temp_directory_path() / "save_allfields.prefab.json";
    std::filesystem::remove(prefabPath);

    const VoidResult saved = VengEditor::PrefabSerialize::Save(*scene, registry, prefabPath);
    REQUIRE_MESSAGE(saved.has_value(), "save failed: ", saved ? string{} : saved.error());

    const Result<vector<u8>> blobResult =
        CookPrefabSource("save_allfields", prefabPath, AssetId{7001}, registry);
    REQUIRE_MESSAGE(blobResult.has_value(),
                    "cook failed: ", blobResult ? string{} : blobResult.error());
    const vector<u8>& blob = *blobResult;

    CookedPrefabHeader header{};
    std::memcpy(&header, blob.data(), sizeof(header));
    CHECK(header.EntityCount == 2);

    // The root is emitted first (hierarchy order: root, then its child).
    const vector<u8> allRecord = ComponentRecord(blob, 0, TypeIdOf<AllFields>());
    REQUIRE_FALSE(allRecord.empty());

    AllFields decoded;
    REQUIRE(ReadFields(allRecord, &decoded, registry.Info(TypeIdOf<AllFields>()), registry));

    CHECK(decoded.Flag == true);                     // Scalar (bool)
    CHECK(decoded.Real == doctest::Approx(1.5f));    // Scalar (f32)
    CHECK(decoded.Signed == -7);                     // Scalar (i32)
    CHECK(decoded.Unsigned == 42u);                  // Scalar (u32)
    CHECK(decoded.Big == 0xABCDEF0123456789ULL);     // Scalar (u64)
    CHECK(decoded.Vector == vec3(1.0f, 2.0f, 3.0f)); // Vector
    // Quaternion (glm quat(w,x,y,z); storage order [x,y,z,w]).
    CHECK(decoded.Quaternion.x == doctest::Approx(0.0f));
    CHECK(decoded.Quaternion.y == doctest::Approx(0.7071f));
    CHECK(decoded.Quaternion.z == doctest::Approx(0.0f));
    CHECK(decoded.Quaternion.w == doctest::Approx(0.7071f));
    CHECK(decoded.Text == "hello");                            // String
    CHECK(decoded.Mesh.Id().Value == 0ULL);                    // AssetHandle
    CHECK(decoded.Enum == LightType::Spot);                    // Enum
    REQUIRE(decoded.Shape.ActiveType() == TypeIdOf<Sphere>()); // Variant
    CHECK(static_cast<const Sphere*>(decoded.Shape.ActivePtr())->Radius == doctest::Approx(0.5f));

    // Reference: the child's Hierarchy.Parent cooks to the root's prefab-local index (0).
    const vector<u8> hierRecord = ComponentRecord(blob, 1, TypeIdOf<Hierarchy>());
    REQUIRE_FALSE(hierRecord.empty());
    Hierarchy hier;
    REQUIRE(ReadFields(hierRecord, &hier, registry.Info(TypeIdOf<Hierarchy>()), registry));
    CHECK(hier.Parent.Index == 0u);
    CHECK(hier.Parent.Generation == 0u);
}

TEST_CASE("prefab save: an AssetHandle field round-trips its decimal id")
{
    const TypeRegistry registry = BuildRegistry();

    const auto scene = Scene::Create(const_cast<TypeRegistry&>(registry));
    const Entity entity = scene->CreateEntity();
    auto& all = scene->Add<AllFields>(entity);

    // Construct a handle carrying a concrete AssetId: the id lives at offset 0, which the
    // writer emits as a decimal unsigned integer and BindField reads back.
    const u64 meshId = 8001ULL;
    std::memcpy(static_cast<void*>(&all.Mesh), &meshId, sizeof(meshId));

    const path prefabPath = std::filesystem::temp_directory_path() / "save_handle.prefab.json";
    std::filesystem::remove(prefabPath);

    REQUIRE(VengEditor::PrefabSerialize::Save(*scene, registry, prefabPath).has_value());

    // The saved JSON carries the decimal id (the JSON convention), not hex.
    const json doc = ReadDoc(prefabPath);
    REQUIRE(doc["entities"].is_array());
    const json& comp = doc["entities"][0]["components"]["AllFields"];
    CHECK(comp["Mesh"].is_number_unsigned());
    CHECK(comp["Mesh"].get<u64>() == meshId);

    const Result<vector<u8>> blobResult =
        CookPrefabSource("save_handle", prefabPath, AssetId{7002}, registry);
    REQUIRE_MESSAGE(blobResult.has_value(),
                    "cook failed: ", blobResult ? string{} : blobResult.error());

    AllFields decoded;
    const vector<u8> record = ComponentRecord(*blobResult, 0, TypeIdOf<AllFields>());
    REQUIRE(ReadFields(record, &decoded, registry.Info(TypeIdOf<AllFields>()), registry));
    CHECK(decoded.Mesh.Id().Value == meshId);
}

TEST_CASE("prefab save: the Array arm emits a JSON array of its elements")
{
    const TypeRegistry registry = BuildRegistry();

    const auto scene = Scene::Create(const_cast<TypeRegistry&>(registry));
    const Entity entity = scene->CreateEntity();
    scene->Add<ArrayHolder>(entity) = ArrayHolder{.Values = {1.0f, 2.0f, 3.0f}};

    const path prefabPath = std::filesystem::temp_directory_path() / "save_array.prefab.json";
    std::filesystem::remove(prefabPath);

    REQUIRE(VengEditor::PrefabSerialize::Save(*scene, registry, prefabPath).has_value());

    // The Array arm writes a JSON array of its elements (the inverse of an array read), walked
    // through the descriptor's type-erased element shims. Prefab sources do not cook array fields,
    // so this arm is validated on the written JSON rather than through the cook.
    const json doc = ReadDoc(prefabPath);
    REQUIRE(doc["entities"].is_array());
    const json& values = doc["entities"][0]["components"]["ArrayHolder"]["Values"];
    REQUIRE(values.is_array());
    REQUIRE(values.size() == 3);
    CHECK(values[0].get<f32>() == doctest::Approx(1.0f));
    CHECK(values[1].get<f32>() == doctest::Approx(2.0f));
    CHECK(values[2].get<f32>() == doctest::Approx(3.0f));
}

TEST_CASE("prefab save: an unknown hand-authored key survives a save")
{
    const TypeRegistry registry = BuildRegistry();

    const auto scene = Scene::Create(const_cast<TypeRegistry&>(registry));
    const Entity entity = scene->CreateEntity();
    scene->Add<Name>(entity) = Name{.Value = "Kept"};

    const path prefabPath = std::filesystem::temp_directory_path() / "save_unknown.prefab.json";

    // Seed a source whose single entity carries the stable id this entity will save under
    // (slot index 0), an entity-level extra key, and a components-level extra key — neither of
    // which the writer understands, both of which must survive in place.
    {
        json prefab;
        json e;
        e[string{VengEditor::PrefabSerialize::EntityIdKey}] = 0u;
        e["_comment"] = "do not delete this";
        json components;
        components["_componentNote"] = "keep me too";
        e["components"] = std::move(components);
        prefab["entities"] = json::array({e});
        std::ofstream(prefabPath) << prefab.dump();
    }

    REQUIRE(VengEditor::PrefabSerialize::Save(*scene, registry, prefabPath).has_value());

    const json doc = ReadDoc(prefabPath);
    REQUIRE(doc["entities"].is_array());
    REQUIRE(doc["entities"].size() == 1);
    const json& e = doc["entities"][0];
    // Both unknown keys are preserved in place; the component the writer understands is patched.
    CHECK(e.contains("_comment"));
    CHECK(e["_comment"] == "do not delete this");
    CHECK(e["components"].contains("_componentNote"));
    CHECK(e["components"]["_componentNote"] == "keep me too");
    CHECK(e["components"].contains("Veng::Name"));
    CHECK(e["components"]["Veng::Name"]["Value"] == "Kept");
}

TEST_CASE("prefab save: deleting + reordering entities re-aligns by id, preserving unknown keys")
{
    const TypeRegistry registry = BuildRegistry();

    // A parent with three ordered children A, B, C. Slot indices are the stable per-entity ids the
    // writer keys on; the children's order is the meaningful (intrusive-sibling) order to reorder.
    const auto scene = Scene::Create(const_cast<TypeRegistry&>(registry));
    const Entity parent = scene->CreateEntity();
    scene->Add<Name>(parent) = Name{.Value = "Parent"};
    const Entity a = scene->CreateEntity();
    const Entity b = scene->CreateEntity();
    const Entity c = scene->CreateEntity();
    scene->Add<Name>(a) = Name{.Value = "A"};
    scene->Add<Name>(b) = Name{.Value = "B"};
    scene->Add<Name>(c) = Name{.Value = "C"};
    scene->SetParent(a, parent);
    scene->SetParent(b, parent);
    scene->SetParent(c, parent);

    const path prefabPath = std::filesystem::temp_directory_path() / "save_realign.prefab.json";
    std::filesystem::remove(prefabPath);

    // First save establishes the source: four entities keyed by id (slot index).
    REQUIRE(VengEditor::PrefabSerialize::Save(*scene, registry, prefabPath).has_value());

    // Hand-author a per-entity extra key onto each source object, addressed by its id, so the
    // alignment is observable after a structural edit.
    {
        json doc = ReadDoc(prefabPath);
        for (json& e : doc["entities"])
        {
            const u64 id = e[string{VengEditor::PrefabSerialize::EntityIdKey}].get<u64>();
            e["_note"] = fmt::format("entity-{}", id);
        }
        std::ofstream(prefabPath) << doc.dump();
    }

    const u64 idA = a.Index;
    const u64 idC = c.Index;

    // Delete B and reorder so C precedes A among the parent's children: the positional order no
    // longer matches the source array, so only id-keyed alignment keeps each survivor's note.
    scene->DestroyEntity(b);
    scene->MoveBefore(c, a);

    REQUIRE(VengEditor::PrefabSerialize::Save(*scene, registry, prefabPath).has_value());

    const json doc = ReadDoc(prefabPath);
    REQUIRE(doc["entities"].is_array());
    REQUIRE(doc["entities"].size() == 3); // parent + A + C; B was dropped

    // Build an id → note map from the re-saved source and check each survivor kept its own note,
    // i.e. the writer matched live entity to source object by id, not by position.
    std::map<u64, string> noteById;
    for (const json& e : doc["entities"])
    {
        const u64 id = e[string{VengEditor::PrefabSerialize::EntityIdKey}].get<u64>();
        REQUIRE(e.contains("_note"));
        noteById[id] = e["_note"].get<string>();
    }

    CHECK(noteById.count(idA) == 1);
    CHECK(noteById.count(idC) == 1);
    CHECK(noteById[idA] == fmt::format("entity-{}", idA));
    CHECK(noteById[idC] == fmt::format("entity-{}", idC));

    // The emitted order is hierarchy order: parent, then its reordered children C then A. B's id
    // is gone entirely.
    REQUIRE(doc["entities"][0]["components"].contains("Veng::Name"));
    CHECK(doc["entities"][0]["components"]["Veng::Name"]["Value"] == "Parent");
    CHECK(doc["entities"][1][string{VengEditor::PrefabSerialize::EntityIdKey}].get<u64>() == idC);
    CHECK(doc["entities"][2][string{VengEditor::PrefabSerialize::EntityIdKey}].get<u64>() == idA);
}
