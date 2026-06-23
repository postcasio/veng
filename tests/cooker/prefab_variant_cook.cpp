// Cooker variant-binding test: cooks a *.prefab.json that authors a variant
// field as { "type", "value" } through the PrefabImporter against a hand-built
// TypeRegistry holding a variant-bearing component. Covers the valid path (the
// blob round-trips through the engine ReadFields to the chosen alternative +
// value), the bad-tag located error, a nested-field located error, the omitted
// form, and the explicit-empty form (byte-identical to omission). Pure CPU — no
// Context, no Vulkan symbol touched.

#include <cstring>
#include <filesystem>
#include <fstream>

#include <doctest/doctest.h>

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Reflection/Variant.h>

using namespace Veng;
using namespace Veng::Cook;

// ---- A throwaway variant-bearing component for the cooker to validate -------

namespace
{
    struct BoxShape
    {
        f32 Width = 0.0f;
        f32 Height = 0.0f;
    };

    struct SphereShape
    {
        f32 Radius = 0.0f;
    };

    using ShapeVariant = Variant<BoxShape, SphereShape>;

    struct ShapeComponent
    {
        ShapeVariant Shape;
        i32 Trailing = 0;
    };
}

VE_REFLECT(::BoxShape, 0x4B1C8F2A6D90E573ULL)
VE_FIELD(Width)
VE_FIELD(Height)
VE_REFLECT_END();

VE_REFLECT(::SphereShape, 0x9E2D7A4C13B6F085ULL)
VE_FIELD(Radius)
VE_REFLECT_END();

VE_VARIANT(::ShapeVariant, 0x2F5A91D3C7E40B68ULL);

VE_REFLECT(::ShapeComponent, 0x86C0E4B7159D2A3FULL)
VE_FIELD(Shape)
VE_FIELD(Trailing)
VE_REFLECT_END();

namespace
{
    // A registry holding the variant component and its alternatives.
    TypeRegistry BuildRegistry()
    {
        TypeRegistry registry;
        registry.Register<ShapeComponent>();
        registry.Register<ShapeVariant>();
        return registry;
    }

    // Writes a one-entity prefab JSON whose single ShapeComponent carries the
    // given component-fields JSON, plus a one-entry prefab pack, returning the
    // pack path. The variant component is keyed by its registered name.
    path WriteVariantPrefab(const string& name, const json& componentFields)
    {
        const path dir = std::filesystem::temp_directory_path();
        const path prefabPath = dir / (name + ".prefab.json");
        const path packPath = dir / (name + ".pack.json");

        json components;
        components["ShapeComponent"] = componentFields;

        json entity;
        entity["name"] = "E";
        entity["components"] = components;

        json prefab;
        prefab["entities"] = json::array({entity});
        std::ofstream(prefabPath) << prefab.dump();

        json asset;
        asset["id"] = 4242;
        asset["type"] = "prefab";
        asset["source"] = prefabPath.filename().string();

        json pack;
        pack["version"] = 1;
        pack["assets"] = json::array({asset});
        std::ofstream(packPath) << pack.dump();

        return packPath;
    }

    // Cooks a prefab whose single component is a ShapeComponent with the given
    // fields, returning the cooked prefab blob (or the located error).
    Result<vector<u8>> CookVariantPrefab(const string& name, const json& componentFields,
                                         const TypeRegistry& registry)
    {
        const path packPath = WriteVariantPrefab(name, componentFields);

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

        const optional<ArchiveEntry> archived = reader->Find(AssetId{4242});
        if (!archived.has_value())
        {
            return std::unexpected(string("prefab entry missing from archive"));
        }

        return vector<u8>(archived->Blob.begin(), archived->Blob.end());
    }

    // Slices the single component's record bytes out of a cooked prefab blob.
    vector<u8> ComponentRecord(const vector<u8>& blob)
    {
        CookedPrefabHeader header{};
        std::memcpy(&header, blob.data(), sizeof(header));
        REQUIRE(header.EntityCount == 1);
        REQUIRE(header.ComponentCount == 1);

        const auto* component = reinterpret_cast<const CookedPrefabComponent*>(
            blob.data() + sizeof(CookedPrefabHeader) + sizeof(CookedPrefabEntity));
        const u8* records = blob.data() + sizeof(CookedPrefabHeader) + sizeof(CookedPrefabEntity) +
                            sizeof(CookedPrefabComponent);

        return vector<u8>(records + component->RecordOffset,
                          records + component->RecordOffset + component->RecordSize);
    }
}

TEST_CASE("prefab variant cook: a valid { type, value } round-trips to the active alternative")
{
    const TypeRegistry registry = BuildRegistry();

    json shape;
    shape["type"] = "SphereShape";
    shape["value"] = json::object({{"Radius", 0.5}});
    json fields;
    fields["Shape"] = shape;
    fields["Trailing"] = 7;

    const Result<vector<u8>> blob = CookVariantPrefab("prefab_variant_valid", fields, registry);
    REQUIRE_MESSAGE(blob.has_value(), "cook failed: ", blob ? string{} : blob.error());

    const vector<u8> record = ComponentRecord(*blob);
    ShapeComponent decoded;
    REQUIRE(ReadFields(record, &decoded, registry.Info(TypeIdOf<ShapeComponent>()), registry));
    REQUIRE(decoded.Shape.ActiveType() == TypeIdOf<SphereShape>());
    CHECK(static_cast<const SphereShape*>(decoded.Shape.ActivePtr())->Radius ==
          doctest::Approx(0.5f));
    CHECK(decoded.Trailing == 7);
}

TEST_CASE("prefab variant cook: an unknown 'type' is a located error naming the field and variant")
{
    const TypeRegistry registry = BuildRegistry();

    json shape;
    shape["type"] = "NotAShape";
    json fields;
    fields["Shape"] = shape;

    const Result<vector<u8>> blob = CookVariantPrefab("prefab_variant_badtag", fields, registry);
    REQUIRE_FALSE(blob.has_value());
    CHECK(blob.error().find("Shape") != string::npos);
    CHECK(blob.error().find("NotAShape") != string::npos);
    CHECK(blob.error().find("is not an alternative") != string::npos);
}

TEST_CASE("prefab variant cook: a nested field not on the chosen alternative is a located error")
{
    const TypeRegistry registry = BuildRegistry();

    json shape;
    shape["type"] = "SphereShape";
    shape["value"] = json::object({{"Width", 1.0}}); // Width is on BoxShape, not SphereShape
    json fields;
    fields["Shape"] = shape;

    const Result<vector<u8>> blob = CookVariantPrefab("prefab_variant_badnested", fields, registry);
    REQUIRE_FALSE(blob.has_value());
    CHECK(blob.error().find("Width") != string::npos);
    CHECK(blob.error().find("SphereShape") != string::npos);
}

TEST_CASE("prefab variant cook: an omitted variant field loads empty")
{
    const TypeRegistry registry = BuildRegistry();

    json fields;
    fields["Trailing"] = 3; // Shape is omitted entirely

    const Result<vector<u8>> blob = CookVariantPrefab("prefab_variant_omitted", fields, registry);
    REQUIRE_MESSAGE(blob.has_value(), "cook failed: ", blob ? string{} : blob.error());

    const vector<u8> record = ComponentRecord(*blob);
    ShapeComponent decoded;
    static_cast<BoxShape*>(decoded.Shape.SetActive(TypeIdOf<BoxShape>()))->Width = 9.0f; // pre-fill
    REQUIRE(ReadFields(record, &decoded, registry.Info(TypeIdOf<ShapeComponent>()), registry));
    CHECK_FALSE(decoded.Shape.HasValue());
    CHECK(decoded.Trailing == 3);
}

TEST_CASE("prefab variant cook: explicit empty { type: \"\" } is byte-identical to omission")
{
    const TypeRegistry registry = BuildRegistry();

    json omittedFields;
    omittedFields["Trailing"] = 11;

    json emptyShape;
    emptyShape["type"] = "";
    json emptyFields;
    emptyFields["Shape"] = emptyShape;
    emptyFields["Trailing"] = 11;

    const Result<vector<u8>> omitted =
        CookVariantPrefab("prefab_variant_empty_omit", omittedFields, registry);
    const Result<vector<u8>> explicitEmpty =
        CookVariantPrefab("prefab_variant_empty_explicit", emptyFields, registry);
    REQUIRE(omitted.has_value());
    REQUIRE(explicitEmpty.has_value());

    const vector<u8> omittedRecord = ComponentRecord(*omitted);
    const vector<u8> explicitRecord = ComponentRecord(*explicitEmpty);
    CHECK(omittedRecord == explicitRecord);

    ShapeComponent decoded;
    REQUIRE(
        ReadFields(explicitRecord, &decoded, registry.Info(TypeIdOf<ShapeComponent>()), registry));
    CHECK_FALSE(decoded.Shape.HasValue());
    CHECK(decoded.Trailing == 11);
}
