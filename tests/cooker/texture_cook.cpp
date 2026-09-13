// Texture cook test: cooks a fixture texture pack through
// libveng_cook and checks the resulting CookedTextureHeader (assetpack) and
// pixel bytes match the fixture's source PNG + .tex.json.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <filesystem>
#include "support/TempPath.h"
#include <random>

#include <doctest/doctest.h>
#include <fmt/format.h>
#include <glm/gtc/packing.hpp>

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Renderer/Types.h>

using namespace Veng;
using namespace Veng::Cook;

namespace
{
    // Cooks a single-texture pack with no build configuration and returns a copy of the written
    // texture blob. The archive name is unique per call so two cases cannot cook over each other.
    vector<u8> CookTextureBlob(const path& packJson, const AssetId id)
    {
        std::random_device rng;
        const path outArchive = Veng::TestSupport::TempDir() /
                                fmt::format("veng_cooker_texture_{:08x}.vengpack", rng());

        Cooker cooker;
        RegisterBuiltinImporters(cooker);
        REQUIRE(cooker.CookPack(packJson, outArchive).has_value());

        const Result<ArchiveReader> reader = ArchiveReader::Open(outArchive);
        REQUIRE(reader.has_value());

        const optional<ArchiveEntry> entry = reader->Find(id);
        REQUIRE(entry.has_value());
        REQUIRE(entry->Blob.size() >= sizeof(CookedTextureHeader));

        const vector<u8> blob(entry->Blob.begin(), entry->Blob.end());
        std::filesystem::remove(outArchive);
        return blob;
    }

    // The header a cooked texture blob opens with.
    CookedTextureHeader HeaderOf(const vector<u8>& blob)
    {
        CookedTextureHeader header{};
        std::memcpy(&header, blob.data(), sizeof(header));
        return header;
    }

    // The half-float value at `index` within the level beginning at `offset`.
    f32 HalfAt(const vector<u8>& blob, usize offset, usize index)
    {
        u16 bits = 0;
        std::memcpy(&bits, blob.data() + offset + index * sizeof(u16), sizeof(bits));
        return glm::unpackHalf1x16(bits);
    }

    // The tightly-packed byte size of a half-float mip chain storing `channels` channels.
    usize HalfChainBytes(const CookedTextureHeader& header, u32 channels)
    {
        usize total = 0;
        for (u32 level = 0; level < header.MipCount; level++)
        {
            total += static_cast<usize>(std::max(1u, header.Width >> level)) *
                     std::max(1u, header.Height >> level) * channels * sizeof(u16);
        }
        return total;
    }
}

TEST_CASE("Cooker: cooks a texture pack into a CookedTextureHeader + RGBA8 pixels")
{
    const path fixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
    const path packJson = fixtureDir / "texture_pack.json";
    const path outArchive = Veng::TestSupport::TempDir() / "veng_cooker_texture.vengpack";

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    const VoidResult cookResult = cooker.CookPack(packJson, outArchive);
    REQUIRE(cookResult.has_value());

    const Result<ArchiveReader> reader = ArchiveReader::Open(outArchive);
    REQUIRE(reader.has_value());

    const optional<ArchiveEntry> entry = reader->Find(AssetId{0x7D1});
    REQUIRE(entry.has_value());
    CHECK(entry->Type == AssetTypes::Texture);

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

    CHECK(header.Version == CookedTextureVersion);
    // A single-mip, non-sRGB (Mask-role default) texture is never mip-cappable.
    CHECK(header.MipCappable == 0);

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
    const path outArchive = Veng::TestSupport::TempDir() / "veng_cooker_texture_mipped.vengpack";

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    const VoidResult cookResult = cooker.CookPack(packJson, outArchive);
    REQUIRE(cookResult.has_value());

    const Result<ArchiveReader> reader = ArchiveReader::Open(outArchive);
    REQUIRE(reader.has_value());

    const optional<ArchiveEntry> entry = reader->Find(AssetId{0x6725A9A1089EF916});
    REQUIRE(entry.has_value());
    CHECK(entry->Type == AssetTypes::Texture);

    REQUIRE(entry->Blob.size() >= sizeof(CookedTextureHeader));

    CookedTextureHeader header{};
    std::memcpy(&header, entry->Blob.data(), sizeof(header));

    CHECK(header.Width == 8);
    CHECK(header.Height == 8);
    // An 8x8 source halves through 8, 4, 2, 1 — four levels.
    CHECK(header.MipCount == 4);
    // A multi-mip texture whose role is the non-sRGB Mask default is still not cappable — the cap
    // gate is the role, not the presence of mips.
    CHECK(header.MipCappable == 0);

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
    const path outArchive = Veng::TestSupport::TempDir() / "veng_cooker_texture_bc7.vengpack";

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    const VoidResult cookResult = cooker.CookPack(packJson, outArchive);
    REQUIRE(cookResult.has_value());

    const Result<ArchiveReader> reader = ArchiveReader::Open(outArchive);
    REQUIRE(reader.has_value());

    const optional<ArchiveEntry> entry = reader->Find(AssetId{0x3B7ECB6D353F7974ULL});
    REQUIRE(entry.has_value());
    CHECK(entry->Type == AssetTypes::Texture);

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
    const path outArchive = Veng::TestSupport::TempDir() / "veng_cooker_texture_astc.vengpack";

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    const VoidResult cookResult = cooker.CookPack(packJson, outArchive);
    REQUIRE(cookResult.has_value());

    const Result<ArchiveReader> reader = ArchiveReader::Open(outArchive);
    REQUIRE(reader.has_value());

    const optional<ArchiveEntry> entry = reader->Find(AssetId{0xD8C88B8D55FEEB1BULL});
    REQUIRE(entry.has_value());
    CHECK(entry->Type == AssetTypes::Texture);

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

TEST_CASE("Cooker: the half-float ordinals match Renderer::Format")
{
    // The same hand-synced contract the BC7/ASTC pins guard: the cooker writes 5/6/18 and the
    // engine's BridgeFormat reads them back into these exact enumerators.
    CHECK(static_cast<u32>(Renderer::Format::R16Sfloat) == 5);
    CHECK(static_cast<u32>(Renderer::Format::RGBA16Sfloat) == 6);
    CHECK(static_cast<u32>(Renderer::Format::RG16Sfloat) == 18);
}

TEST_CASE("Cooker: an EXR source cooks to RGBA16Sfloat with no configuration")
{
    const path fixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
    const vector<u8> blob = CookTextureBlob(fixtureDir / "texture_float_rgba_pack.json",
                                            AssetId{0x881592C1898B753CULL});
    const CookedTextureHeader header = HeaderOf(blob);

    // A float source has no eight-bit fallback, so it reaches the half-float family even where an
    // eight-bit source would take the zero-config ASTC default.
    CHECK(header.Format == static_cast<u32>(Renderer::Format::RGBA16Sfloat));
    CHECK(header.Width == 8);
    CHECK(header.Height == 4);
    // An 8x4 source halves through 8x4, 4x2, 2x1, 1x1 — four levels.
    CHECK(header.MipCount == 4);
    CHECK(header.ChannelLayout == static_cast<u32>(CookedChannelLayout::Direct));
    REQUIRE(blob.size() == sizeof(CookedTextureHeader) + HalfChainBytes(header, 4));

    // The fixture's values — R alternating 0.25/0.75 by column, a constant 0.5 in G, a 0.25-a-row
    // ramp in B, 1.0 in A — are all exact in half, so the round trip is lossless.
    const usize base = sizeof(CookedTextureHeader);
    f32 maxError = 0.0f;
    for (u32 y = 0; y < header.Height; y++)
    {
        for (u32 x = 0; x < header.Width; x++)
        {
            const usize texel = (static_cast<usize>(y) * header.Width + x) * 4;
            const f32 expected[4] = {(x & 1u) != 0 ? 0.75f : 0.25f, 0.5f,
                                     0.25f * static_cast<f32>(y), 1.0f};
            for (usize c = 0; c < 4; c++)
            {
                maxError =
                    std::max(maxError, std::abs(HalfAt(blob, base, texel + c) - expected[c]));
            }
        }
    }
    CHECK(maxError < 1e-6f);
}

TEST_CASE("Cooker: a float mip level holds the box mean of its parent's tones")
{
    const path fixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
    const vector<u8> blob = CookTextureBlob(fixtureDir / "texture_float_rgba_pack.json",
                                            AssetId{0x881592C1898B753CULL});
    const CookedTextureHeader header = HeaderOf(blob);
    REQUIRE(header.MipCount >= 2);

    // Level 1 is the 2x2 box mean of level 0, taken in float before the half conversion: R's two
    // column tones (0.25 and 0.75) average to 0.5 everywhere. A chain filtered from quantised
    // parents could not land on the mean of the tones.
    const usize level1 =
        sizeof(CookedTextureHeader) + static_cast<usize>(header.Width) * header.Height * 4 * 2;
    const u32 levelWidth = std::max(1u, header.Width >> 1);
    const u32 levelHeight = std::max(1u, header.Height >> 1);

    f32 maxError = 0.0f;
    for (usize texel = 0; texel < static_cast<usize>(levelWidth) * levelHeight; texel++)
    {
        maxError = std::max(maxError, std::abs(HalfAt(blob, level1, texel * 4) - 0.5f));
    }
    CHECK(maxError < 1e-6f);
}

TEST_CASE("Cooker: a one-channel EXR infers R16Sfloat")
{
    const path fixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
    const vector<u8> blob =
        CookTextureBlob(fixtureDir / "texture_float_r_pack.json", AssetId{0x84F4E2BDF7356BFCULL});
    const CookedTextureHeader header = HeaderOf(blob);

    CHECK(header.Format == static_cast<u32>(Renderer::Format::R16Sfloat));
    CHECK(header.Width == 4);
    CHECK(header.Height == 4);
    CHECK(header.MipCount == 3);
    // One half a texel a level, not four: the source's width picks the family's member.
    REQUIRE(blob.size() == sizeof(CookedTextureHeader) + HalfChainBytes(header, 1));
    CHECK(HalfAt(blob, sizeof(CookedTextureHeader), 0) == doctest::Approx(0.5f));
}

TEST_CASE("Cooker: \"channels\" narrows a four-channel float source to RG16Sfloat")
{
    const path fixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
    const vector<u8> blob = CookTextureBlob(fixtureDir / "texture_float_rgba_rg_pack.json",
                                            AssetId{0x0D645C71831A93DDULL});
    const CookedTextureHeader header = HeaderOf(blob);

    CHECK(header.Format == static_cast<u32>(Renderer::Format::RG16Sfloat));
    REQUIRE(blob.size() == sizeof(CookedTextureHeader) + HalfChainBytes(header, 2));

    // The narrowed source keeps R and G and drops B and A.
    const usize base = sizeof(CookedTextureHeader);
    CHECK(HalfAt(blob, base, 0) == doctest::Approx(0.25f));
    CHECK(HalfAt(blob, base, 1) == doctest::Approx(0.5f));
}

TEST_CASE("Cooker: a two-channel 16-bit PNG infers RG16Sfloat and scales 65535 to 1.0")
{
    const path fixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
    const vector<u8> blob =
        CookTextureBlob(fixtureDir / "texture_float_rg_pack.json", AssetId{0x4C3558CBA109B2D1ULL});
    const CookedTextureHeader header = HeaderOf(blob);

    CHECK(header.Format == static_cast<u32>(Renderer::Format::RG16Sfloat));
    CHECK(header.Width == 8);
    CHECK(header.Height == 4);
    REQUIRE(blob.size() == sizeof(CookedTextureHeader) + HalfChainBytes(header, 2));

    // The u16 range maps onto [0, 1]: the fixture's full-scale R reads back as exactly 1.0.
    const usize base = sizeof(CookedTextureHeader);
    CHECK(HalfAt(blob, base, 0) == doctest::Approx(1.0f));
    CHECK(HalfAt(blob, base, 1) == doctest::Approx(0.5f));
}

TEST_CASE("Cooker: \"srgb\" on a float source is refused")
{
    const path fixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
    const path outArchive =
        Veng::TestSupport::TempDir() / "veng_cooker_texture_float_srgb.vengpack";

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    // A float texture is linear by definition, so an sRGB flag on one is an authoring mistake the
    // cook names rather than silently ignores.
    const VoidResult cookResult =
        cooker.CookPack(fixtureDir / "texture_float_srgb_pack.json", outArchive);
    REQUIRE_FALSE(cookResult.has_value());
    CHECK(cookResult.error().find("texture_float_srgb.tex.json") != string::npos);
    CHECK(cookResult.error().find("srgb") != string::npos);

    std::filesystem::remove(outArchive);
}
