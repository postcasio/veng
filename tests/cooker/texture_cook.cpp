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
#include <Veng/Renderer/Types.h>

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

TEST_CASE("Cooker: cooks a BC7 texture to Format 21/22 with the expected per-level block sizes")
{
    const path fixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
    const path packJson = fixtureDir / "texture_bc7_pack.json";
    const path outArchive =
        std::filesystem::temp_directory_path() / "veng_cooker_texture_bc7.vengpack";

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    const VoidResult cookResult = cooker.CookPack(packJson, outArchive);
    REQUIRE(cookResult.has_value());

    const Result<ArchiveReader> reader = ArchiveReader::Open(outArchive);
    REQUIRE(reader.has_value());

    const optional<ArchiveEntry> entry = reader->Find(AssetId{0x3B7ECB6D353F7974ULL});
    REQUIRE(entry.has_value());
    CHECK(entry->Type == AssetType::Texture);

    REQUIRE(entry->Blob.size() >= sizeof(CookedTextureHeader));

    CookedTextureHeader header{};
    std::memcpy(&header, entry->Blob.data(), sizeof(header));

    // The fixture is a linear (srgb: false) BC7 texture: Format ordinal 21 = BC7Unorm. The ordinal
    // is hand-synced to Renderer::Format, so assert against the enum to catch a transposition.
    CHECK(header.Format == 21);
    CHECK(header.Format == static_cast<u32>(Renderer::Format::BC7Unorm));
    CHECK(header.Width == 8);
    CHECK(header.Height == 8);
    // An 8x8 source halves through 8, 4, 2, 1 — four levels.
    CHECK(header.MipCount == 4);

    // Each level is BC7 blocks: ceil(w/4) * ceil(h/4) * 16 bytes. The 2x2 and 1x1 levels are
    // partial edge blocks padded to a full 4x4 block — the non-multiple-of-4 case.
    usize expectedBlockBytes = 0;
    for (u32 level = 0; level < header.MipCount; level++)
    {
        const u32 levelWidth = std::max(1u, header.Width >> level);
        const u32 levelHeight = std::max(1u, header.Height >> level);
        const u32 blocksWide = (levelWidth + 3) / 4;
        const u32 blocksHigh = (levelHeight + 3) / 4;
        expectedBlockBytes += static_cast<usize>(blocksWide) * blocksHigh * 16;
    }
    // 8x8 -> 4 blocks (64B); 4x4 -> 1 (16B); 2x2 -> 1 (16B); 1x1 -> 1 (16B): 112 bytes total.
    CHECK(expectedBlockBytes == 112);
    REQUIRE(entry->Blob.size() == sizeof(CookedTextureHeader) + expectedBlockBytes);

    std::filesystem::remove(outArchive);
}

TEST_CASE("Cooker: the BC7 sRGB/Unorm ordinals match Renderer::Format")
{
    // Guards the hand-synced cycle-avoidance contract: the cooker writes 21/22 and the engine's
    // BridgeFormat reads them back into these exact Renderer::Format enumerators. A transposition
    // (21 <-> 22) would slip past a self-consistent round-trip but fail this ordinal pin.
    CHECK(static_cast<u32>(Renderer::Format::BC7Unorm) == 21);
    CHECK(static_cast<u32>(Renderer::Format::BC7Srgb) == 22);
}

TEST_CASE("Cooker: cooks an ASTC texture to Format 23/24 with the expected per-level block sizes")
{
    const path fixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
    const path packJson = fixtureDir / "texture_astc_pack.json";
    const path outArchive =
        std::filesystem::temp_directory_path() / "veng_cooker_texture_astc.vengpack";

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    const VoidResult cookResult = cooker.CookPack(packJson, outArchive);
    REQUIRE(cookResult.has_value());

    const Result<ArchiveReader> reader = ArchiveReader::Open(outArchive);
    REQUIRE(reader.has_value());

    const optional<ArchiveEntry> entry = reader->Find(AssetId{0xD8C88B8D55FEEB1BULL});
    REQUIRE(entry.has_value());
    CHECK(entry->Type == AssetType::Texture);

    REQUIRE(entry->Blob.size() >= sizeof(CookedTextureHeader));

    CookedTextureHeader header{};
    std::memcpy(&header, entry->Blob.data(), sizeof(header));

    // The fixture is a linear (srgb: false) ASTC texture: Format ordinal 23 = ASTC4x4Unorm. The
    // ordinal is hand-synced to Renderer::Format, so assert against the enum to catch a
    // transposition.
    CHECK(header.Format == 23);
    CHECK(header.Format == static_cast<u32>(Renderer::Format::ASTC4x4Unorm));
    CHECK(header.Width == 8);
    CHECK(header.Height == 8);
    // An 8x8 source halves through 8, 4, 2, 1 — four levels.
    CHECK(header.MipCount == 4);

    // Each level is ASTC 4x4 blocks: ceil(w/4) * ceil(h/4) * 16 bytes — the same block geometry as
    // BC7. The 2x2 and 1x1 levels are partial edge blocks padded to a full 4x4 block.
    usize expectedBlockBytes = 0;
    for (u32 level = 0; level < header.MipCount; level++)
    {
        const u32 levelWidth = std::max(1u, header.Width >> level);
        const u32 levelHeight = std::max(1u, header.Height >> level);
        const u32 blocksWide = (levelWidth + 3) / 4;
        const u32 blocksHigh = (levelHeight + 3) / 4;
        expectedBlockBytes += static_cast<usize>(blocksWide) * blocksHigh * 16;
    }
    // 8x8 -> 4 blocks (64B); 4x4 -> 1 (16B); 2x2 -> 1 (16B); 1x1 -> 1 (16B): 112 bytes total.
    CHECK(expectedBlockBytes == 112);
    REQUIRE(entry->Blob.size() == sizeof(CookedTextureHeader) + expectedBlockBytes);

    std::filesystem::remove(outArchive);
}

TEST_CASE("Cooker: the ASTC sRGB/Unorm ordinals match Renderer::Format")
{
    // Guards the hand-synced cycle-avoidance contract: the cooker writes 23/24 and the engine's
    // BridgeFormat reads them back into these exact Renderer::Format enumerators. A transposition
    // (23 <-> 24) would slip past a self-consistent round-trip but fail this ordinal pin.
    CHECK(static_cast<u32>(Renderer::Format::ASTC4x4Unorm) == 23);
    CHECK(static_cast<u32>(Renderer::Format::ASTC4x4Srgb) == 24);
}
