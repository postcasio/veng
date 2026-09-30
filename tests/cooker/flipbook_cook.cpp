// Flipbook cook: a flipbook-atlas manifest (schema v1) and its image, synthesized here — a 2x2 grid
// of 4x4 frames — cook into a CookedFlipbookHeader carrying the stated grid and timing plus an
// embedded cooked texture of the atlas's size; a frame-sequence export (no atlas image) and a grid
// that does not tile the atlas are refused.

#include <cstring>
#include <filesystem>
#include <fstream>
#include "support/TempPath.h"

#include <doctest/doctest.h>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>

using namespace Veng;
using namespace Veng::Cook;

namespace
{
    constexpr AssetId FlipbookId{0x378B81EAE35916B1ULL};

    // Writes an uncompressed 32-bit top-left-origin TGA: the image decoder reads it by content, so
    // it stands in for the PNG an atlas tool would write.
    void WriteTga(const path& file, const u32 width, const u32 height)
    {
        std::ofstream out(file, std::ios::binary);
        const u8 header[18] = {0,
                               0,
                               2,
                               0,
                               0,
                               0,
                               0,
                               0,
                               0,
                               0,
                               0,
                               0,
                               static_cast<u8>(width & 0xFF),
                               static_cast<u8>(width >> 8),
                               static_cast<u8>(height & 0xFF),
                               static_cast<u8>(height >> 8),
                               32,
                               0x28};
        out.write(reinterpret_cast<const char*>(header), sizeof(header));
        for (u32 texel = 0; texel < width * height; ++texel)
        {
            const u8 bgra[4] = {40, 120, 200, 255};
            out.write(reinterpret_cast<const char*>(bgra), sizeof(bgra));
        }
    }

    // A schema-v1 manifest over a 2x2 grid of 4x4 frames, with fields the cook ignores.
    nlohmann::json Manifest()
    {
        return {
            {"version", 1},
            {"type", "flipbook-atlas"},
            {"source", "test"},
            {"technique", "volumetric"},
            {"name", "burst"},
            {"atlas", {{"file", "burst.atlas.tga"}, {"width", 8}, {"height", 8}}},
            {"frame", {{"width", 4}, {"height", 4}}},
            {"columns", 2},
            {"rows", 2},
            {"frames", 3},
            {"fps", 12.5},
            {"loop", false},
            {"blend", "additive"},
            {"alpha", "luminance"},
            {"colorSpace", "linear"},
            {"worldExtent", {2.0, 3.0, 4.0}},
            {"pivot", {1.0, 0.0, 2.0}},
            {"provenance", {{"toolVersion", "0.1.0"}, {"seed", 7}}},
        };
    }

    // Cooks one flipbook entry whose manifest is `manifest`, beside a synthesized atlas image.
    Result<vector<u8>> CookFlipbook(const nlohmann::json& manifest, const string& tag)
    {
        const path dir = Veng::TestSupport::TempDir() / fmt::format("flipbook_cook_{}", tag);
        std::filesystem::create_directories(dir);
        WriteTga(dir / "burst.atlas.tga", 8, 8);
        std::ofstream(dir / "burst.atlas.json") << manifest.dump(2);
        const nlohmann::json pack = {
            {"version", 1},
            {"assets",
             {{{"id", "0x378B81EAE35916B1"},
               {"type", "Flipbook"},
               {"source", "burst.atlas.json"}}}},
        };
        std::ofstream(dir / "pack.json") << pack.dump(2);

        Cooker cooker;
        RegisterBuiltinImporters(cooker);
        const path archive = dir / "flipbook.vengpack";
        if (const VoidResult cooked = cooker.CookPack(dir / "pack.json", archive); !cooked)
        {
            return std::unexpected(cooked.error());
        }
        const Result<ArchiveReader> reader = ArchiveReader::Open(archive);
        REQUIRE(reader.has_value());
        const optional<ArchiveEntry> entry = reader->Find(FlipbookId);
        REQUIRE(entry.has_value());
        CHECK(entry->Type == AssetTypes::Flipbook);
        return vector<u8>(entry->Blob.begin(), entry->Blob.end());
    }
}

TEST_CASE("Cooker: a flipbook-atlas manifest cooks its grid, timing, and atlas texture")
{
    const Result<vector<u8>> blob = CookFlipbook(Manifest(), "import");
    REQUIRE(blob.has_value());
    REQUIRE(blob->size() >= sizeof(CookedFlipbookHeader));

    CookedFlipbookHeader header{};
    std::memcpy(&header, blob->data(), sizeof(header));
    CHECK(header.Version == CookedFlipbookVersion);
    CHECK(header.Columns == 2);
    CHECK(header.Rows == 2);
    CHECK(header.FrameCount == 3);
    CHECK(header.FrameWidth == 4);
    CHECK(header.FrameHeight == 4);
    CHECK(header.Fps == doctest::Approx(12.5f));
    CHECK(header.Loop == 0u);
    CHECK(header.Blend == 1u);     // additive
    CHECK(header.AlphaMode == 2u); // luminance
    CHECK(header.HasWorldExtent == 1u);
    CHECK(header.WorldExtent[1] == doctest::Approx(3.0f));
    CHECK(header.HasPivot == 1u);
    CHECK(header.Pivot[2] == doctest::Approx(2.0f));

    // The embedded texture is a whole cooked texture blob of the atlas's size.
    REQUIRE(blob->size() == sizeof(header) + header.TextureBytes);
    CookedTextureHeader texture{};
    std::memcpy(&texture, blob->data() + sizeof(header), sizeof(texture));
    CHECK(texture.Version == CookedTextureVersion);
    CHECK(texture.Width == 8);
    CHECK(texture.Height == 8);
    CHECK(texture.MipCount > 1);
    CHECK(texture.AddressModeU == 2u); // clamp to edge
}

TEST_CASE("Cooker: a frame-sequence export with no atlas image is refused")
{
    nlohmann::json manifest = Manifest();
    manifest["atlas"].erase("file");
    const Result<vector<u8>> blob = CookFlipbook(manifest, "sequence");
    REQUIRE_FALSE(blob.has_value());
    CHECK(blob.error().find("frame-sequence export") != string::npos);
}

TEST_CASE("Cooker: a flipbook grid that does not tile its atlas is refused")
{
    nlohmann::json manifest = Manifest();
    manifest["frame"]["width"] = 3;
    const Result<vector<u8>> blob = CookFlipbook(manifest, "grid");
    REQUIRE_FALSE(blob.has_value());
    CHECK(blob.error().find("is not the 8x8 atlas") != string::npos);
}
