// vengc verify tests: cook a fixture pack, then drive VerifyArchive over the
// cooked archive and over deliberately tampered copies of it. Proves the
// per-blob hash pass, the TOC-digest pass, and the open/version-drift guard,
// each in isolation. Driver-free (libveng_cook only, no GPU).

#include <doctest/doctest.h>

#include <cstring>
#include <filesystem>
#include <fstream>
#include <iterator>

#include <Veng/Asset/Archive.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Cook/Verify.h>

using namespace Veng;
using namespace Veng::Cook;

namespace
{
    // On-disk layout offsets (Archive.h): a 32-byte header — magic[8],
    // version@8, count@12, archiveDigest(16)@16 — then count
    // 56-byte TOC entries: id@0, type@8, codec@12, offset@16, size@24, hash(16)@32,
    // uncompressedSize@48.
    constexpr usize HeaderSize = 32;
    constexpr usize ArchiveDigestOffset = 16;
    constexpr usize TocEntrySize = 56;
    constexpr usize TocEntryHashOffset = 32;

    vector<u8> ReadFile(const path& filePath)
    {
        std::ifstream file(filePath, std::ios::binary);
        REQUIRE(file.is_open());
        return vector<u8>(std::istreambuf_iterator<char>(file), std::istreambuf_iterator<char>());
    }

    void WriteFile(const path& filePath, const vector<u8>& bytes)
    {
        std::ofstream file(filePath, std::ios::binary | std::ios::trunc);
        REQUIRE(file.is_open());
        file.write(reinterpret_cast<const char*>(bytes.data()),
                   static_cast<std::streamsize>(bytes.size()));
    }

    // Cook the raw fixture pack to outArchive and return the cooked bytes.
    vector<u8> CookRawPack(const path& outArchive)
    {
        const path fixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
        const path packJson = fixtureDir / "raw_pack.json";

        Cooker cooker;
        RegisterBuiltinImporters(cooker);

        const VoidResult cookResult = cooker.CookPack(packJson, outArchive);
        REQUIRE(cookResult.has_value());

        return ReadFile(outArchive);
    }

    bool AssetVerdict(const VerifyReport& report, AssetId id)
    {
        for (const VerifiedAsset& asset : report.Assets)
        {
            if (asset.Id.Value == id.Value)
            {
                return asset.Ok;
            }
        }
        FAIL("asset id not present in report");
        return false;
    }
}

TEST_CASE("verify: a freshly cooked pack passes, every asset OK")
{
    const path archive = std::filesystem::temp_directory_path() / "veng_verify_clean.vengpack";
    CookRawPack(archive);

    const VerifyReport report = VerifyArchive(archive);
    CHECK_FALSE(report.OpenError.has_value());
    REQUIRE(report.Assets.size() == 2);
    CHECK(report.Assets[0].Ok);
    CHECK(report.Assets[1].Ok);
    CHECK(report.DigestOk);
    CHECK(report.Ok());

    CHECK(VerifyArchiveCli(archive) == 0);

    std::filesystem::remove(archive);
}

TEST_CASE("verify: a flipped blob byte fails, naming exactly the affected asset")
{
    const path archive = std::filesystem::temp_directory_path() / "veng_verify_blob_src.vengpack";
    vector<u8> bytes = CookRawPack(archive);

    // The intact pack beside it still passes.
    const VerifyReport intact = VerifyArchive(archive);
    CHECK(intact.Ok());

    // Flip the last byte: it lies inside the last blob's region (id 1002, a
    // non-empty blob), so only that asset's per-blob hash should mismatch. The
    // TOC is untouched, so the digest still matches.
    bytes.back() ^= 0xFF;

    const path tampered = std::filesystem::temp_directory_path() / "veng_verify_blob.vengpack";
    WriteFile(tampered, bytes);

    const VerifyReport report = VerifyArchive(tampered);
    CHECK_FALSE(report.OpenError.has_value());
    REQUIRE(report.Assets.size() == 2);
    CHECK(AssetVerdict(report, AssetId{1001}));       // intact blob still OK
    CHECK_FALSE(AssetVerdict(report, AssetId{1002})); // the corrupted one
    CHECK(report.DigestOk);                           // TOC unchanged
    CHECK_FALSE(report.Ok());

    CHECK(VerifyArchiveCli(tampered) == 1);

    std::filesystem::remove(archive);
    std::filesystem::remove(tampered);
}

TEST_CASE("verify: tampering only the stored digest fails the digest, not the blobs")
{
    const path archive = std::filesystem::temp_directory_path() / "veng_verify_digest_src.vengpack";
    vector<u8> bytes = CookRawPack(archive);

    // Flip a byte of the header's stored ArchiveDigest, leaving every blob and
    // the TOC intact: all per-blob hashes still match, but the recomputed TOC
    // digest disagrees with the stored one.
    bytes[ArchiveDigestOffset] ^= 0xFF;

    const path tampered = std::filesystem::temp_directory_path() / "veng_verify_digest.vengpack";
    WriteFile(tampered, bytes);

    const VerifyReport report = VerifyArchive(tampered);
    CHECK_FALSE(report.OpenError.has_value());
    REQUIRE(report.Assets.size() == 2);
    CHECK(report.Assets[0].Ok);
    CHECK(report.Assets[1].Ok);
    CHECK_FALSE(report.DigestOk);
    CHECK_FALSE(report.Ok());

    CHECK(VerifyArchiveCli(tampered) == 1);

    std::filesystem::remove(archive);
    std::filesystem::remove(tampered);
}

TEST_CASE("verify: tampering a TOC entry field fails the digest")
{
    const path archive = std::filesystem::temp_directory_path() / "veng_verify_toc_src.vengpack";
    vector<u8> bytes = CookRawPack(archive);

    // Flip a byte of the first TOC entry's stored per-blob Hash field — a TOC
    // edit the per-blob pass cannot catch on its own (it would just make that
    // blob mismatch, but here we change the stored hash to one that still
    // disagrees, exercising the digest guard over the table of contents).
    bytes[HeaderSize + TocEntryHashOffset] ^= 0xFF;

    const path tampered = std::filesystem::temp_directory_path() / "veng_verify_toc.vengpack";
    WriteFile(tampered, bytes);

    const VerifyReport report = VerifyArchive(tampered);
    CHECK_FALSE(report.OpenError.has_value());
    CHECK_FALSE(report.DigestOk);
    CHECK_FALSE(report.Ok());

    CHECK(VerifyArchiveCli(tampered) == 1);

    std::filesystem::remove(archive);
    std::filesystem::remove(tampered);
}

TEST_CASE("verify: a wrong-version header is a VersionMismatch reported before hashing")
{
    const path archive =
        std::filesystem::temp_directory_path() / "veng_verify_version_src.vengpack";
    vector<u8> bytes = CookRawPack(archive);

    // Bump the header version to one the reader rejects. Header layout: magic[8],
    // version (u32) @ 8.
    const u32 badVersion = ArchiveFormatVersion + 1;
    std::memcpy(bytes.data() + 8, &badVersion, sizeof(badVersion));

    const path tampered = std::filesystem::temp_directory_path() / "veng_verify_version.vengpack";
    WriteFile(tampered, bytes);

    const VerifyReport report = VerifyArchive(tampered);
    REQUIRE(report.OpenError.has_value());
    CHECK(report.OpenError->find("version") != string::npos);
    CHECK(report.Assets.empty()); // no hashing ran
    CHECK_FALSE(report.Ok());

    CHECK(VerifyArchiveCli(tampered) == 1);

    std::filesystem::remove(archive);
    std::filesystem::remove(tampered);
}
