// Cook-on-demand plumbing test: cooks a single texture source asset
// (brick.tex.json) into an in-memory single-entry .vengpack via
// Cooker::CookSource — the path the editor's CookSession drives off the render
// thread — and checks the bytes parse as a valid archive carrying the target id.
// CPU-only: the texture importer never links into libveng.

#include <filesystem>

#include <doctest/doctest.h>

#include <Veng/Asset/Archive.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>

using namespace Veng;
using namespace Veng::Cook;

TEST_CASE("Cooker: CookSource produces a mountable in-memory archive for a texture")
{
    const path source = path(VENG_HT_ASSETS_DIR) / "textures" / "brick.tex.json";
    const AssetId targetId{0xC00C0DE000000001ULL};

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    const Result<vector<u8>> bytes = cooker.CookSource(source, targetId, AssetType::Texture);
    REQUIRE(bytes.has_value());
    CHECK_FALSE(bytes->empty());

    const Result<ArchiveReader> reader = ArchiveReader::FromBytes(*bytes);
    REQUIRE(reader.has_value());

    const optional<ArchiveEntry> entry = reader->Find(targetId);
    REQUIRE(entry.has_value());
    CHECK(entry->Type == AssetType::Texture);
    CHECK_FALSE(entry->Blob.empty());
}

TEST_CASE("Cooker: CookSource on a missing source reports a located error")
{
    const path missing = path(VENG_HT_ASSETS_DIR) / "textures" / "does_not_exist.tex.json";

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    const Result<vector<u8>> bytes = cooker.CookSource(missing, AssetId{0x1}, AssetType::Texture);
    CHECK_FALSE(bytes.has_value());
}
