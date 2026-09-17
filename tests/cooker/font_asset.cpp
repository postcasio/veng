// Font cook: the face-backed blob. A cooked font is the em line metrics, a default field type, the
// face outline bytes, the hot-set codepoints, and the fallback-font id chain — everything the
// runtime needs to rasterize any covered codepoint on demand and resolve a missing one through
// another shipped face. There is no baked atlas or glyph table. The version bump means a blob cooked
// before this layout is rejected at load rather than misread; that reject is exercised in the GPU
// tier where a live loader exists.

#include <cstring>
#include <fstream>
#include <string>

#include "support/TempPath.h"

#include <doctest/doctest.h>

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>

using namespace Veng;
using namespace Veng::Cook;

namespace
{
    constexpr AssetId BaseFontId{0xF0B0};
    constexpr AssetId FallbackFontId{0xF0B1};

    void WriteFile(const path& file, const std::string& text)
    {
        std::ofstream out(file, std::ios::binary | std::ios::trunc);
        out << text;
    }

    path CaseDir(const char* name)
    {
        const path dir = Veng::TestSupport::TempDir() / (std::string("veng_font_asset_") + name);
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        return dir;
    }

    // The cooked font blob for an id, or nullopt if the archive/entry is missing.
    optional<vector<u8>> BlobOf(const path& archive, AssetId id)
    {
        const Result<ArchiveReader> reader = ArchiveReader::Open(archive);
        if (!reader.has_value())
        {
            return std::nullopt;
        }
        const optional<ArchiveEntry> entry = reader->Find(id);
        if (!entry.has_value())
        {
            return std::nullopt;
        }
        return vector<u8>(entry->Blob.begin(), entry->Blob.end());
    }

    CookedFontHeader HeaderOf(const vector<u8>& blob)
    {
        CookedFontHeader header{};
        REQUIRE(blob.size() >= sizeof(header));
        std::memcpy(&header, blob.data(), sizeof(header));
        return header;
    }

    // The byte offset of the fallback id array within a font blob: header, then the face bytes, then
    // the hot-set codepoints.
    usize FallbackOffset(const CookedFontHeader& header)
    {
        return sizeof(header) + static_cast<usize>(header.FaceBytes) +
               static_cast<usize>(header.HotsetCount) * sizeof(u32);
    }
}

TEST_CASE("Cooker: a font embeds its face, field type, hot set, and fallback chain")
{
    const path ttf = path(VENG_DEFAULT_FONT_TTF);
    REQUIRE(std::filesystem::exists(ttf));

    const path dir = CaseDir("chain");

    // The base font declares an SDF field and a fallback to a second Font in the same pack.
    WriteFile(dir / "base.font.json",
              "{\n  \"font\": \"" + ttf.generic_string() +
                  "\",\n  \"field\": \"sdf\",\n  \"hotset\": \"ascii\",\n  \"fallback\": "
                  "[\"0x000000000000F0B1\"],\n  \"glyphSize\": 24,\n  \"pixelRange\": 4\n}\n");
    WriteFile(dir / "fallback.font.json",
              "{\n  \"font\": \"" + ttf.generic_string() +
                  "\",\n  \"hotset\": \"ascii\",\n  \"glyphSize\": 24,\n  \"pixelRange\": 4\n}\n");
    WriteFile(dir / "pack.json",
              R"({"version": 1, "assets": [
                   {"id": "0x000000000000F0B0", "type": "Font", "source": "base.font.json"},
                   {"id": "0x000000000000F0B1", "type": "Font", "source": "fallback.font.json"}]})");

    Cooker cooker;
    RegisterBuiltinImporters(cooker);
    const path out = dir / "chain.vengpack";
    REQUIRE(cooker.CookPack(dir / "pack.json", out).has_value());

    const optional<vector<u8>> baseBlob = BlobOf(out, BaseFontId);
    REQUIRE(baseBlob.has_value());
    const CookedFontHeader base = HeaderOf(*baseBlob);

    // The version reflects the face-backed layout, and the new sections are all present.
    CHECK(base.Version == CookedFontVersion);
    CHECK(base.FieldType == 1u); // sdf
    CHECK(base.FaceBytes > 0u);
    CHECK(base.HotsetCount > 0u);
    REQUIRE(base.FallbackCount == 1u);

    // The face bytes are the actual font file, embedded whole.
    CHECK(base.FaceBytes == std::filesystem::file_size(ttf));

    // The one fallback id round-trips to the second font's id.
    const usize offset = FallbackOffset(base);
    REQUIRE(baseBlob->size() >= offset + sizeof(u64));
    u64 fallbackId = 0;
    std::memcpy(&fallbackId, baseBlob->data() + offset, sizeof(u64));
    CHECK(fallbackId == FallbackFontId.Value);

    // The fallback font defaults to the MSDF field and carries no fallback of its own.
    const optional<vector<u8>> fbBlob = BlobOf(out, FallbackFontId);
    REQUIRE(fbBlob.has_value());
    const CookedFontHeader fb = HeaderOf(*fbBlob);
    CHECK(fb.FieldType == 0u); // msdf default
    CHECK(fb.FallbackCount == 0u);
    CHECK(fb.FaceBytes > 0u);
}

TEST_CASE("Cooker: a fallback naming an unknown or non-font asset is a located error")
{
    const path ttf = path(VENG_DEFAULT_FONT_TTF);
    const path dir = CaseDir("badfallback");

    WriteFile(dir / "base.font.json",
              "{\n  \"font\": \"" + ttf.generic_string() +
                  "\",\n  \"fallback\": [\"0x00000000DEADBEEF\"],\n  \"glyphSize\": 24\n}\n");
    WriteFile(dir / "pack.json",
              R"({"version": 1, "assets": [
                   {"id": "0x000000000000F0B0", "type": "Font", "source": "base.font.json"}]})");

    Cooker cooker;
    RegisterBuiltinImporters(cooker);
    const VoidResult cooked = cooker.CookPack(dir / "pack.json", dir / "out.vengpack");
    REQUIRE_FALSE(cooked.has_value());
    CHECK(cooked.error().find("fallback") != string::npos);
}

TEST_CASE("Cooker: an invalid field type is an error naming the valid ones")
{
    const path ttf = path(VENG_DEFAULT_FONT_TTF);
    const path dir = CaseDir("badfield");

    WriteFile(dir / "base.font.json", "{\n  \"font\": \"" + ttf.generic_string() +
                                          "\",\n  \"field\": \"bogus\",\n  \"glyphSize\": 24\n}\n");
    WriteFile(dir / "pack.json",
              R"({"version": 1, "assets": [
                   {"id": "0x000000000000F0B0", "type": "Font", "source": "base.font.json"}]})");

    Cooker cooker;
    RegisterBuiltinImporters(cooker);
    const VoidResult cooked = cooker.CookPack(dir / "pack.json", dir / "out.vengpack");
    REQUIRE_FALSE(cooked.has_value());
    CHECK(cooked.error().find("msdf") != string::npos);
    CHECK(cooked.error().find("sdf") != string::npos);
}
