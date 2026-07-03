// Device-free cases for AssetSourceIndex's type-filtered enumeration: the
// candidate set the inspector's AssetHandle picker offers. Writes a small temp
// manifest, parses it, and checks EntriesOfType filters by AssetType.

#include <doctest/doctest.h>

#include "AssetSourceIndex.h"

#include <Veng/Asset/AssetType.h>

#include <algorithm>
#include <fstream>

using namespace VengEditor;
using Veng::AssetType;

namespace
{
    Veng::path WriteTempManifest()
    {
        const Veng::path dir = std::filesystem::temp_directory_path();
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
    const AssetSourceIndex index = AssetSourceIndex::Parse(manifest);

    const Veng::vector<Veng::AssetId> textures = index.EntriesOfType(AssetType::Texture);
    CHECK(textures.size() == 2);
    CHECK(Contains(textures, 1001));
    CHECK(Contains(textures, 1002));
    CHECK_FALSE(Contains(textures, 2001));

    const Veng::vector<Veng::AssetId> materials = index.EntriesOfType(AssetType::Material);
    CHECK(materials.size() == 1);
    CHECK(Contains(materials, 2001));

    const Veng::vector<Veng::AssetId> meshes = index.EntriesOfType(AssetType::Mesh);
    CHECK(meshes.size() == 1);
    CHECK(Contains(meshes, 3001));

    // A type with no manifest entries yields an empty candidate set.
    CHECK(index.EntriesOfType(AssetType::Shader).empty());

    std::error_code ec;
    std::filesystem::remove(manifest, ec);
}
