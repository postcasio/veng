// Device-free cases for AssetSourceIndex's type-filtered enumeration: the
// candidate set the inspector's AssetHandle picker offers. Writes a small temp
// manifest, parses it, and checks EntriesOfType filters by AssetTypeId.

#include <doctest/doctest.h>
#include <Veng/Path.h>
#include "support/TempPath.h"

#include "AssetSourceIndex.h"

#include <Veng/Asset/AssetHandle.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Asset/DataTable.h>
#include <Veng/Reflection/TypeId.h>

#include <algorithm>
#include <fstream>

using namespace VengEditor;
using Veng::AssetTypeId;
namespace AssetTypes = Veng::AssetTypes;

namespace
{
    Veng::path WriteTempManifest()
    {
        const Veng::path dir = Veng::TestSupport::TempDir();
        const Veng::path manifest = dir / "veng_editor_source_index_test.vengpack.json";

        // Two textures, one material, one mesh — distinct ids per type.
        std::ofstream out(manifest, std::ios::binary | std::ios::trunc);
        out << R"({
  "assets": [
    { "id": "0x00000000000003E9", "type": "Texture",  "source": "a.tex.json" },
    { "id": "0x00000000000003EA", "type": "Texture",  "source": "b.tex.json" },
    { "id": "0x00000000000007D1", "type": "Material", "source": "m.vmat.json" },
    { "id": "0x0000000000000BB9", "type": "Mesh",     "source": "x.mesh.json" }
  ]
})";
        return manifest;
    }

    bool Contains(const Veng::vector<Veng::AssetId>& ids, Veng::u64 value)
    {
        return std::ranges::any_of(ids, [value](Veng::AssetId id) { return id.Value == value; });
    }
}

TEST_CASE("AssetSourceIndex: EntriesOfType filters candidates by asset type")
{
    const Veng::path manifest = WriteTempManifest();
    Veng::AssetTypeRegistry assetTypes;
    Veng::RegisterBuiltinAssetTypes(assetTypes);
    const AssetSourceIndex index = AssetSourceIndex::Parse(manifest, assetTypes);

    const Veng::vector<Veng::AssetId> textures = index.EntriesOfType(AssetTypes::Texture);
    CHECK(textures.size() == 2);
    CHECK(Contains(textures, 1001));
    CHECK(Contains(textures, 1002));
    CHECK_FALSE(Contains(textures, 2001));

    const Veng::vector<Veng::AssetId> materials = index.EntriesOfType(AssetTypes::Material);
    CHECK(materials.size() == 1);
    CHECK(Contains(materials, 2001));

    const Veng::vector<Veng::AssetId> meshes = index.EntriesOfType(AssetTypes::Mesh);
    CHECK(meshes.size() == 1);
    CHECK(Contains(meshes, 3001));

    // A type with no manifest entries yields an empty candidate set.
    CHECK(index.EntriesOfType(AssetTypes::Shader).empty());

    std::error_code ec;
    std::filesystem::remove(manifest, ec);
}

TEST_CASE("AssetSourceIndex: a table handle field offers exactly the tables as candidates")
{
    // The picker's whole resolution in the order DrawAssetPicker performs it: a field's leaf
    // TypeId → the asset type that claims it → that type's manifest entries. No widget code sits
    // between those two lookups, so this is the candidate set the inspector offers.
    const Veng::path dir = Veng::TestSupport::TempDir();
    const Veng::path manifest = dir / "veng_editor_table_index_test.vengpack.json";
    {
        std::ofstream out(manifest, std::ios::binary | std::ios::trunc);
        out << R"({
  "assets": [
    { "id": "0x0000000000001389", "type": "TableSchema", "source": "t.tableschema.json" },
    { "id": "0x0000000000001771", "type": "DataTable",   "source": "t.table.json" },
    { "id": "0x0000000000001B59", "type": "Texture",     "source": "a.tex.json" }
  ]
})";
    }

    Veng::AssetTypeRegistry assetTypes;
    Veng::RegisterBuiltinAssetTypes(assetTypes);
    const AssetSourceIndex index = AssetSourceIndex::Parse(manifest, assetTypes);

    const Veng::optional<AssetTypeId> tableType =
        assetTypes.FindByHandleField(Veng::TypeIdOf<Veng::AssetHandle<Veng::DataTable>>());
    REQUIRE(tableType.has_value());
    CHECK(*tableType == AssetTypes::DataTable);

    const Veng::vector<Veng::AssetId> tables = index.EntriesOfType(*tableType);
    REQUIRE(tables.size() == 1);
    CHECK(Contains(tables, 0x1771));

    const Veng::optional<AssetTypeId> schemaType =
        assetTypes.FindByHandleField(Veng::TypeIdOf<Veng::AssetHandle<Veng::TableSchema>>());
    REQUIRE(schemaType.has_value());
    const Veng::vector<Veng::AssetId> schemas = index.EntriesOfType(*schemaType);
    REQUIRE(schemas.size() == 1);
    CHECK(Contains(schemas, 0x1389));

    std::error_code ec;
    std::filesystem::remove(manifest, ec);
}
