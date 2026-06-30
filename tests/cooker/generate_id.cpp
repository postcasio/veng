// AssetId minting + the `.vmat`-declared default-instance companion.
//
// Covers the in-process id generator the material editor mints through
// (GenerateAssetId over reference pack manifest paths) and the cook rule that a
// parent material whose .vmat.json declares a `defaultInstance` id emits a
// companion zero-override MaterialInstance at that id.

#include <array>
#include <filesystem>

#include <doctest/doctest.h>

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Cook/AssetPack.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>

using namespace Veng;
using namespace Veng::Cook;

namespace
{
    const path FixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
}

TEST_CASE("Cooker: GenerateAssetId mints a collision-free id against reference packs")
{
    const std::array<path, 1> references{FixtureDir / "mesh_pack.json"};
    const Result<AssetId> minted = GenerateAssetId(references);
    REQUIRE(minted.has_value());
    CHECK(minted->IsValid());

    // The minted id collides with no entry in the referenced pack.
    const Result<AssetPack> pack = ParseAssetPack(references[0]);
    REQUIRE(pack.has_value());
    CHECK(pack->FindById(*minted) == nullptr);
}

TEST_CASE("Cooker: GenerateAssetId surfaces a parse error for a missing reference pack")
{
    const std::array<path, 1> references{FixtureDir / "does_not_exist.json"};
    const Result<AssetId> minted = GenerateAssetId(references);
    CHECK_FALSE(minted.has_value());
}

TEST_CASE("Cooker: a .vmat's defaultInstance id emits a companion MaterialInstance")
{
    // mesh_pack's brick.vmat.json declares defaultInstance 9001003; the cook emits the
    // companion zero-override instance at that id beside the parent Material (1003).
    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    const path outArchive =
        std::filesystem::temp_directory_path() / "veng_cooker_default_instance.vengpack";
    const VoidResult cooked =
        cooker.CookPack(FixtureDir / "mesh_pack.json", outArchive, {}, nullptr, nullptr, nullptr,
                        nullptr, {}, path(VENG_CORE_SHADER_DIR));
    REQUIRE_MESSAGE(cooked.has_value(), cooked.error());

    const Result<ArchiveReader> reader = ArchiveReader::Open(outArchive);
    REQUIRE(reader.has_value());

    const optional<ArchiveEntry> parent = reader->Find(AssetId{1003});
    REQUIRE(parent.has_value());
    CHECK(parent->Type == AssetType::Material);

    const optional<ArchiveEntry> companion = reader->Find(AssetId{9001003});
    REQUIRE(companion.has_value());
    CHECK(companion->Type == AssetType::MaterialInstance);
}
