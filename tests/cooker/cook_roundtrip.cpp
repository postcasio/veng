// Cooker round-trip test: cooks a fixture pack of `raw`
// entries end-to-end through libveng_cook, then reads the resulting archive
// back with assetpack's ArchiveReader and checks ids/types/bytes match.
// Round-trips the whole tool's pack -> CookPack -> ArchiveWriter -> archive
// -> ArchiveReader chain.

#include <doctest/doctest.h>

#include <algorithm>
#include <filesystem>
#include <fstream>
#include <iterator>

#include <Veng/Asset/Archive.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>

using namespace Veng;
using namespace Veng::Cook;

namespace
{
    vector<u8> ReadFile(const path& filePath)
    {
        std::ifstream file(filePath, std::ios::binary);
        REQUIRE(file.is_open());
        return vector<u8>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }
}

TEST_CASE("Cooker: cooks a raw-only pack into an archive ArchiveReader accepts")
{
    const path fixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
    const path packJson = fixtureDir / "raw_pack.json";
    const path outArchive = std::filesystem::temp_directory_path() / "veng_cooker_roundtrip.vengpack";

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    const VoidResult cookResult = cooker.CookPack(packJson, outArchive);
    REQUIRE(cookResult.has_value());

    const Result<ArchiveReader> reader = ArchiveReader::Open(outArchive);
    REQUIRE(reader.has_value());

    CHECK(reader->Entries().size() == 2);

    const optional<ArchiveEntry> alpha = reader->Find(AssetId{0x3E9});
    REQUIRE(alpha.has_value());
    CHECK(alpha->Type == AssetType::Raw);
    CHECK(std::ranges::equal(alpha->Blob, ReadFile(fixtureDir / "data" / "alpha.bin")));

    const optional<ArchiveEntry> beta = reader->Find(AssetId{0x3EA});
    REQUIRE(beta.has_value());
    CHECK(beta->Type == AssetType::Raw);
    CHECK(std::ranges::equal(beta->Blob, ReadFile(fixtureDir / "data" / "beta.bin")));

    std::filesystem::remove(outArchive);
}

TEST_CASE("Cooker: duplicate ids are a located cook error")
{
    const path fixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
    const path packJson = fixtureDir / "duplicate_id_pack.json";
    const path outArchive = std::filesystem::temp_directory_path() / "veng_cooker_dup.vengpack";

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    const VoidResult cookResult = cooker.CookPack(packJson, outArchive);
    REQUIRE_FALSE(cookResult.has_value());
    CHECK(cookResult.error().find("duplicated") != string::npos);
}

TEST_CASE("Cooker: unknown asset type is a located cook error")
{
    const path fixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
    const path packJson = fixtureDir / "unknown_type_pack.json";
    const path outArchive = std::filesystem::temp_directory_path() / "veng_cooker_unknown_type.vengpack";

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    const VoidResult cookResult = cooker.CookPack(packJson, outArchive);
    REQUIRE_FALSE(cookResult.has_value());
    CHECK(cookResult.error().find("unknown type 'txture'") != string::npos);
}
