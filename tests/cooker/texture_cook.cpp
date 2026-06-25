// Texture cook test: cooks a fixture texture pack through
// libveng_cook and checks the resulting CookedTextureHeader (assetpack) and
// pixel bytes match the fixture's source PNG + .tex.json.

#include <algorithm>
#include <cstring>
#include <filesystem>

#include <doctest/doctest.h>

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>

using namespace Veng;
using namespace Veng::Cook;

TEST_CASE("Cooker: cooks a texture pack into a CookedTextureHeader + RGBA8 pixels")
{
    const path fixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
    const path packJson = fixtureDir / "texture_pack.json";
    const path outArchive = std::filesystem::temp_directory_path() / "veng_cooker_texture.vengpack";

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    const VoidResult cookResult = cooker.CookPack(packJson, outArchive);
    REQUIRE(cookResult.has_value());

    const Result<ArchiveReader> reader = ArchiveReader::Open(outArchive);
    REQUIRE(reader.has_value());

    const optional<ArchiveEntry> entry = reader->Find(AssetId{0x7D1});
    REQUIRE(entry.has_value());
    CHECK(entry->Type == AssetType::Texture);

    REQUIRE(entry->Blob.size() >= sizeof(CookedTextureHeader));

    CookedTextureHeader header{};
    std::memcpy(&header, entry->Blob.data(), sizeof(header));

    CHECK(header.Format == 2); // RGBA8Unorm ("srgb": false)
    CHECK(header.Width == 4);
    CHECK(header.Height == 4);
    CHECK(header.MipCount == 1); // fixture sets "generate_mips": false

    CHECK(header.MinFilter == 0);    // Nearest
    CHECK(header.MagFilter == 0);    // Nearest
    CHECK(header.MipmapMode == 0);   // Nearest
    CHECK(header.AddressModeU == 2); // ClampToEdge
    CHECK(header.AddressModeV == 2); // ClampToEdge
    CHECK(header.AddressModeW == 0); // Repeat (default, not set in fixture)
    CHECK(header.AnisotropyEnabled == 0);
    CHECK(header.MaxAnisotropy == 1.0f);

    const usize pixelBytes = static_cast<usize>(header.Width) * header.Height * 4;
    REQUIRE(entry->Blob.size() == sizeof(CookedTextureHeader) + pixelBytes);

    const u8* pixels = entry->Blob.data() + sizeof(CookedTextureHeader);
    for (usize i = 0; i < header.Width * header.Height; i++)
    {
        CHECK(pixels[i * 4 + 0] == 200);
        CHECK(pixels[i * 4 + 1] == 80);
        CHECK(pixels[i * 4 + 2] == 40);
        CHECK(pixels[i * 4 + 3] == 255);
    }

    std::filesystem::remove(outArchive);
}

TEST_CASE("Cooker: generates a full mip chain by default and packs it largest-first")
{
    const path fixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
    const path packJson = fixtureDir / "texture_mipped_pack.json";
    const path outArchive =
        std::filesystem::temp_directory_path() / "veng_cooker_texture_mipped.vengpack";

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    const VoidResult cookResult = cooker.CookPack(packJson, outArchive);
    REQUIRE(cookResult.has_value());

    const Result<ArchiveReader> reader = ArchiveReader::Open(outArchive);
    REQUIRE(reader.has_value());

    const optional<ArchiveEntry> entry = reader->Find(AssetId{0x6725A9A1089EF916});
    REQUIRE(entry.has_value());
    CHECK(entry->Type == AssetType::Texture);

    REQUIRE(entry->Blob.size() >= sizeof(CookedTextureHeader));

    CookedTextureHeader header{};
    std::memcpy(&header, entry->Blob.data(), sizeof(header));

    CHECK(header.Width == 8);
    CHECK(header.Height == 8);
    // An 8x8 source halves through 8, 4, 2, 1 — four levels.
    CHECK(header.MipCount == 4);

    // The blob is the header followed by every level tightly packed largest-first; each level's
    // size derives from its halved dimensions, with no offset table.
    usize expectedPixelBytes = 0;
    for (u32 level = 0; level < header.MipCount; level++)
    {
        const u32 levelWidth = std::max(1u, header.Width >> level);
        const u32 levelHeight = std::max(1u, header.Height >> level);
        expectedPixelBytes += static_cast<usize>(levelWidth) * levelHeight * 4;
    }
    REQUIRE(entry->Blob.size() == sizeof(CookedTextureHeader) + expectedPixelBytes);

    // The source is a solid color, so every texel of every level is that color.
    const u8* pixels = entry->Blob.data() + sizeof(CookedTextureHeader);
    for (usize i = 0; i < expectedPixelBytes / 4; i++)
    {
        CHECK(pixels[i * 4 + 0] == 200);
        CHECK(pixels[i * 4 + 1] == 80);
        CHECK(pixels[i * 4 + 2] == 40);
        CHECK(pixels[i * 4 + 3] == 255);
    }

    std::filesystem::remove(outArchive);
}
