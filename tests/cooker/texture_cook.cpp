// Texture cook test: cooks a fixture texture pack through
// libveng_cook and checks the resulting CookedTextureHeader (assetformat) and
// pixel bytes match the fixture's source PNG + .tex.json.

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

    const optional<ArchiveEntry> entry = reader->Find(AssetId{2001});
    REQUIRE(entry.has_value());
    CHECK(entry->Type == AssetType::Texture);

    REQUIRE(entry->Blob.size() >= sizeof(CookedTextureHeader));

    CookedTextureHeader header{};
    std::memcpy(&header, entry->Blob.data(), sizeof(header));

    CHECK(header.Format == 2); // RGBA8Unorm ("srgb": false)
    CHECK(header.Width == 4);
    CHECK(header.Height == 4);
    CHECK(header.MipCount == 1);

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
