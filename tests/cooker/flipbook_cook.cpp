// Flipbook cook: flipbook-atlas manifests and their images, synthesized here, cook into a
// CookedFlipbookHeader and the embedded textures their schema asks for. A schema-1 manifest records
// its grid, timing and one atlas; a schema-2 manifest records its shading, the textures sharing its
// grid at the codec each one's data needs, its emission ramp and its per-frame extent. Malformed
// manifests are refused by a message naming the field, and the texture keys the flipbook cook
// relies on ("alpha", "source_channel", and the per-role BC7 weights) hold their properties.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <filesystem>
#include <fstream>
#include "support/TempPath.h"

#include <bc7decomp.h>
#include <doctest/doctest.h>
#include <fmt/format.h>
#include <nlohmann/json.hpp>
#include <rgbcx.h>

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Project/BuildConfiguration.h>
#include <Veng/Renderer/Types.h>

using namespace Veng;
using namespace Veng::Cook;

namespace
{
    constexpr AssetId FlipbookId{0x378B81EAE35916B1ULL};
    constexpr AssetId TextureId{0x9ADAC83D7D0F4515ULL};

    // The largest error, in stored 8-bit units, BC4 makes on this test's temperature front, whose
    // 4x4 blocks span up to ~85 units. BC7 or ASTC alpha holding the same field errs by 22 to 26.
    constexpr i32 TemperatureErrorBound = 6;

    using Texel = std::array<u8, 4>;

    // Writes an uncompressed 32-bit top-left-origin TGA whose texel (x, y) is `texel(x, y)` as
    // RGBA: the image decoder reads it by content, so it stands in for a tool's PNG.
    template <class TexelFn>
    void WriteTga(const path& file, const u32 width, const u32 height, TexelFn texel)
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
        for (u32 y = 0; y < height; ++y)
        {
            for (u32 x = 0; x < width; ++x)
            {
                const Texel rgba = texel(x, y);
                const u8 bgra[4] = {rgba[2], rgba[1], rgba[0], rgba[3]};
                out.write(reinterpret_cast<const char*>(bgra), sizeof(bgra));
            }
        }
    }

    // A flat texel, for an image whose content no assertion reads.
    Texel Flat(u32 /*x*/, u32 /*y*/)
    {
        return {40, 120, 200, 255};
    }

    // The windows fixture configuration: BC7 for colour, BC5 for vectors, BC4 for masks.
    const BuildConfiguration& WindowsConfig()
    {
        static const BuildConfiguration config = []
        {
            Result<BuildConfiguration> parsed =
                ParseBuildConfiguration(path(VENG_COOKER_TEST_FIXTURE_DIR) / "windows.buildcfg");
            REQUIRE(parsed.has_value());
            return *parsed;
        }();
        return config;
    }

    // Cooks a one-entry pack in `dir` whose entry is `type` at `source`, and returns its blob.
    Result<vector<u8>> CookEntry(const path& dir, const AssetId id, const char* type,
                                 const string& source, const BuildConfiguration* config)
    {
        const nlohmann::json pack = {
            {"version", 1},
            {"assets",
             {{{"id", fmt::format("0x{:016X}", id.Value)}, {"type", type}, {"source", source}}}},
        };
        std::ofstream(dir / "pack.json") << pack.dump(2);

        Cooker cooker;
        RegisterBuiltinImporters(cooker);
        const path archive = dir / "out.vengpack";
        const path configFile =
            config != nullptr ? path(VENG_COOKER_TEST_FIXTURE_DIR) / "windows.buildcfg" : path();
        if (const VoidResult cooked = cooker.CookPack(dir / "pack.json", archive, {}, nullptr,
                                                      nullptr, nullptr, config, configFile);
            !cooked)
        {
            return std::unexpected(cooked.error());
        }
        const Result<ArchiveReader> reader = ArchiveReader::Open(archive);
        REQUIRE(reader.has_value());
        const optional<ArchiveEntry> entry = reader->Find(id);
        REQUIRE(entry.has_value());
        return vector<u8>(entry->Blob.begin(), entry->Blob.end());
    }

    // A fresh scratch directory for one cook.
    path CaseDir(const string& tag)
    {
        const path dir = Veng::TestSupport::TempDir() / fmt::format("flipbook_cook_{}", tag);
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        return dir;
    }

    // A schema-1 manifest over a 2x2 grid of 4x4 frames, with fields the cook ignores.
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

    // One schema-2 texture entry over the 8x8 grid.
    nlohmann::json Entry(const char* file, const char* colorSpace)
    {
        return {{"file", file}, {"width", 8}, {"height", 8}, {"colorSpace", colorSpace}};
    }

    // A schema-2 six-way manifest over a 2x2 grid of 4x4 frames, with motion and a three-key ramp.
    nlohmann::json SixWayManifest()
    {
        nlohmann::json motion = Entry("motion.tga", "linear");
        motion["scale"] = 0.25;
        return {
            {"version", 2},
            {"type", "flipbook-atlas"},
            {"name", "blast"},
            {"frame", {{"width", 4}, {"height", 4}}},
            {"columns", 2},
            {"rows", 2},
            {"frames", 4},
            {"fps", 24},
            {"loop", false},
            {"blend", "alpha"},
            {"worldExtent", {3.0, 3.0, 3.0}},
            {"shading", "sixway"},
            {"textures",
             {{"lightA", Entry("lightA.tga", "srgb")},
              {"lightB", Entry("lightB.tga", "srgb")},
              {"motion", motion}}},
            {"emission",
             {{"ignition", 0.2},
              {"ramp",
               {{{"t", 0.2}, {"color", {0.5, 0.1, 0.0}}},
                {{"t", 0.6}, {"color", {4.0, 1.5, 0.3}}},
                {{"t", 1.0}, {"color", {20.0, 14.0, 8.0}}}}}}},
        };
    }

    // Cooks a flipbook manifest beside 8x8 images for every file a manifest above names.
    Result<vector<u8>> CookFlipbook(const nlohmann::json& manifest, const string& tag,
                                    const BuildConfiguration* config = nullptr)
    {
        const path dir = CaseDir(tag);
        for (const char* file :
             {"burst.atlas.tga", "lightA.tga", "lightB.tga", "motion.tga", "color.tga"})
        {
            WriteTga(dir / file, 8, 8, Flat);
        }
        std::ofstream(dir / "burst.atlas.json") << manifest.dump(2);
        return CookEntry(dir, FlipbookId, "Flipbook", "burst.atlas.json", config);
    }

    CookedFlipbookHeader FlipbookHeaderOf(const vector<u8>& blob)
    {
        REQUIRE(blob.size() >= sizeof(CookedFlipbookHeader));
        CookedFlipbookHeader header{};
        std::memcpy(&header, blob.data(), sizeof(header));
        return header;
    }

    // The byte offset of `slot`'s embedded texture blob.
    usize SlotOffset(const CookedFlipbookHeader& header, const u32 slot)
    {
        usize offset = sizeof(header);
        if (header.HasFrameScale != 0)
        {
            offset += static_cast<usize>(header.FrameCount) * sizeof(f32);
        }
        for (u32 before = 0; before < slot; ++before)
        {
            offset += header.TextureBytes[before];
        }
        return offset;
    }

    CookedTextureHeader TextureHeaderAt(const vector<u8>& blob, const usize offset)
    {
        REQUIRE(blob.size() >= offset + sizeof(CookedTextureHeader));
        CookedTextureHeader header{};
        std::memcpy(&header, blob.data() + offset, sizeof(header));
        return header;
    }

    constexpr u32 FormatOrdinal(const Renderer::Format format)
    {
        return static_cast<u32>(format);
    }
}

TEST_CASE("Cooker: a flipbook-atlas manifest cooks its grid, timing, and atlas texture")
{
    const Result<vector<u8>> blob = CookFlipbook(Manifest(), "import");
    REQUIRE(blob.has_value());
    const CookedFlipbookHeader header = FlipbookHeaderOf(*blob);
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

    // A schema-1 sheet is an unlit, stepped flipbook with one texture and no emission.
    CHECK(header.Shading == 0u);
    CHECK(header.FrameBlend == 0u);
    CHECK(header.EmissionKeyCount == 0u);
    CHECK(header.HasFrameScale == 0u);
    CHECK(header.TextureBytes[1] == 0u);
    CHECK(header.TextureBytes[2] == 0u);
    CHECK(header.TextureBytes[3] == 0u);

    // The embedded texture is a whole cooked texture blob of the atlas's size.
    REQUIRE(blob->size() == sizeof(header) + header.TextureBytes[0]);
    const CookedTextureHeader texture = TextureHeaderAt(*blob, sizeof(header));
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

TEST_CASE("Cooker: a six-way flipbook cooks every texture at the codec its data needs")
{
    const Result<vector<u8>> blob = CookFlipbook(SixWayManifest(), "sixway", &WindowsConfig());
    REQUIRE(blob.has_value());
    const CookedFlipbookHeader header = FlipbookHeaderOf(*blob);
    CHECK(header.Version == CookedFlipbookVersion);
    CHECK(header.Shading == 1u);
    CHECK(header.FrameBlend == 2u);          // motion
    CHECK(header.TemperatureEncoding == 1u); // sqrt, the default
    CHECK(header.AlphaMode == 0u);           // premultiplied
    CHECK(header.MotionScale == doctest::Approx(0.25f));
    CHECK(header.Ignition == doctest::Approx(0.2f));
    REQUIRE(header.EmissionKeyCount == 3u);
    CHECK(header.EmissionKeys[0][0] == doctest::Approx(0.2f));
    CHECK(header.EmissionKeys[1][1] == doctest::Approx(4.0f));
    CHECK(header.EmissionKeys[2][0] == doctest::Approx(1.0f));
    CHECK(header.EmissionKeys[2][3] == doctest::Approx(8.0f));
    CHECK(header.HasFrameScale == 0u);

    // Slots 0-2 carry the lightmaps and motion, and the temperature cooks into slot 3 of its own.
    usize total = sizeof(header);
    for (u32 slot = 0; slot < CookedFlipbookTextureSlots; ++slot)
    {
        CAPTURE(slot);
        REQUIRE(header.TextureBytes[slot] > 0u);
        total += header.TextureBytes[slot];
        const CookedTextureHeader texture = TextureHeaderAt(*blob, SlotOffset(header, slot));
        CHECK(texture.Width == 8);
        CHECK(texture.Height == 8);
    }
    CHECK(blob->size() == total);

    CHECK(TextureHeaderAt(*blob, SlotOffset(header, 0)).Format ==
          FormatOrdinal(Renderer::Format::BC7Srgb));
    CHECK(TextureHeaderAt(*blob, SlotOffset(header, 1)).Format ==
          FormatOrdinal(Renderer::Format::BC7Srgb));
    const CookedTextureHeader motion = TextureHeaderAt(*blob, SlotOffset(header, 2));
    CHECK(motion.Format == FormatOrdinal(Renderer::Format::BC5Unorm));
    CHECK(motion.VarianceFormat == 0u);
    CHECK(motion.ChannelLayout == static_cast<u32>(CookedChannelLayout::Direct));
    CHECK(TextureHeaderAt(*blob, SlotOffset(header, 3)).Format ==
          FormatOrdinal(Renderer::Format::BC4Unorm));
}

TEST_CASE("Cooker: a six-way flipbook records its per-frame extent")
{
    nlohmann::json manifest = SixWayManifest();
    manifest["frameScale"] = {0.25, 0.5, 0.75, 1.0};
    const Result<vector<u8>> blob = CookFlipbook(manifest, "framescale");
    REQUIRE(blob.has_value());
    const CookedFlipbookHeader header = FlipbookHeaderOf(*blob);
    REQUIRE(header.HasFrameScale == 1u);

    std::array<f32, 4> scales{};
    std::memcpy(scales.data(), blob->data() + sizeof(header), sizeof(scales));
    CHECK(scales == std::array<f32, 4>{0.25f, 0.5f, 0.75f, 1.0f});

    usize total = sizeof(header) + sizeof(scales);
    for (const u32 bytes : header.TextureBytes)
    {
        total += bytes;
    }
    CHECK(blob->size() == total);
}

TEST_CASE("Cooker: an unlit schema-2 flipbook cooks its colour sheet alone")
{
    nlohmann::json manifest = SixWayManifest();
    manifest["shading"] = "unlit";
    manifest["textures"] = {{"color", Entry("color.tga", "srgb")}};
    manifest.erase("emission");
    const Result<vector<u8>> blob = CookFlipbook(manifest, "unlit");
    REQUIRE(blob.has_value());
    const CookedFlipbookHeader header = FlipbookHeaderOf(*blob);
    CHECK(header.Shading == 0u);
    CHECK(header.FrameBlend == 1u); // crossfade: no motion
    CHECK(header.EmissionKeyCount == 0u);
    CHECK(header.MotionScale == 0.0f);
    CHECK(header.TextureBytes[0] > 0u);
    CHECK(header.TextureBytes[1] == 0u);
    CHECK(header.TextureBytes[2] == 0u);
    CHECK(header.TextureBytes[3] == 0u);
    CHECK(blob->size() == sizeof(header) + header.TextureBytes[0]);
}

TEST_CASE("Cooker: a malformed schema-2 flipbook is refused by a message naming the field")
{
    struct Mutation
    {
        const char* Name;
        void (*Apply)(nlohmann::json&);
        const char* Field;
    };
    const Mutation mutations[] = {
        {.Name = "lightB at another size",
         .Apply = [](nlohmann::json& m) { m["textures"]["lightB"]["height"] = 4; },
         .Field = "textures.lightB"},
        {.Name = "lightA missing",
         .Apply = [](nlohmann::json& m) { m["textures"].erase("lightA"); },
         .Field = "textures.lightA"},
        {.Name = "color on a six-way flipbook",
         .Apply = [](nlohmann::json& m) { m["textures"]["color"] = Entry("color.tga", "srgb"); },
         .Field = "textures.color"},
        {.Name = "an unknown texture",
         .Apply = [](nlohmann::json& m) { m["textures"]["lighta"] = Entry("lightA.tga", "srgb"); },
         .Field = "textures.lighta"},
        {.Name = "sRGB motion",
         .Apply = [](nlohmann::json& m) { m["textures"]["motion"]["colorSpace"] = "srgb"; },
         .Field = "textures.motion.colorSpace"},
        {.Name = "motion without a scale",
         .Apply = [](nlohmann::json& m) { m["textures"]["motion"].erase("scale"); },
         .Field = "'scale'"},
        {.Name = "frameScale of the wrong length",
         .Apply = [](nlohmann::json& m) { m["frameScale"] = {1.0, 1.0}; },
         .Field = "frameScale"},
        {.Name = "a frameScale entry of 0",
         .Apply = [](nlohmann::json& m) { m["frameScale"] = {0.0, 1.0, 1.0, 1.0}; },
         .Field = "frameScale"},
        {.Name = "a frameScale entry above 1",
         .Apply = [](nlohmann::json& m) { m["frameScale"] = {1.5, 1.0, 1.0, 1.0}; },
         .Field = "frameScale"},
        {.Name = "alpha present",
         .Apply = [](nlohmann::json& m) { m["alpha"] = "premultiplied"; },
         .Field = "'alpha'"},
        {.Name = "a ramp key below the one before it",
         .Apply = [](nlohmann::json& m)
         { m["emission"]["ramp"].push_back({{"t", 0.9}, {"color", {1.0, 1.0, 1.0}}}); },
         .Field = "emission.ramp[3].t"},
        {.Name = "nine ramp keys",
         .Apply =
             [](nlohmann::json& m)
         {
             nlohmann::json ramp = nlohmann::json::array();
             for (int key = 0; key < 9; ++key)
             {
                 ramp.push_back({{"t", key / 8.0}, {"color", {1.0, 1.0, 1.0}}});
             }
             m["emission"]["ramp"] = ramp;
         },
         .Field = "emission.ramp"},
        {.Name = "a negative colour",
         .Apply = [](nlohmann::json& m) { m["emission"]["ramp"][1]["color"][2] = -0.5; },
         .Field = "emission.ramp[1].color"},
        {.Name = "ignition of 1.5",
         .Apply = [](nlohmann::json& m) { m["emission"]["ignition"] = 1.5; },
         .Field = "emission.ignition"},
        {.Name = "shading lit",
         .Apply = [](nlohmann::json& m) { m["shading"] = "lit"; },
         .Field = "shading"},
        {.Name = "version 3",
         .Apply = [](nlohmann::json& m) { m["version"] = 3; },
         .Field = "version"},
    };
    for (const Mutation& mutation : mutations)
    {
        CAPTURE(mutation.Name);
        nlohmann::json manifest = SixWayManifest();
        mutation.Apply(manifest);
        const Result<vector<u8>> blob = CookFlipbook(manifest, "malformed");
        REQUIRE_FALSE(blob.has_value());
        CAPTURE(blob.error());
        CHECK(blob.error().find(mutation.Field) != string::npos);
    }

    nlohmann::json versionOne = Manifest();
    versionOne["frameScale"] = {1.0, 1.0, 1.0};
    const Result<vector<u8>> blob = CookFlipbook(versionOne, "v1_framescale");
    REQUIRE_FALSE(blob.has_value());
    CHECK(blob.error().find("frameScale") != string::npos);
}

TEST_CASE("Cooker: a six-way flipbook's temperature survives the cook within the measured bound")
{
    // A radial sqrt-temperature ramp with one steep front, behind unrelated lighting.
    constexpr u32 Size = 64;
    const auto stored = [](const u32 x, const u32 y) -> u8
    {
        const f32 r = std::hypot((static_cast<f32>(x) + 0.5f) / Size - 0.5f,
                                 (static_cast<f32>(y) + 0.5f) / Size - 0.5f) /
                      0.5f;
        const f32 t = std::clamp(1.0f - r, 0.0f, 1.0f);
        f32 steep = 0.0f;
        if (t < 0.25f)
        {
            steep = t / 0.25f * 0.15f;
        }
        else if (t <= 0.45f)
        {
            steep = 0.15f + ((t - 0.25f) / 0.2f * 0.7f);
        }
        else
        {
            steep = 0.85f + ((t - 0.45f) / 0.55f * 0.15f);
        }
        return static_cast<u8>(std::lround(std::sqrt(steep) * 255.0f));
    };

    const path dir = CaseDir("temperature");
    WriteTga(dir / "lightA.tga", Size, Size, Flat);
    WriteTga(dir / "lightB.tga", Size, Size,
             [&](const u32 x, const u32 y) -> Texel
             {
                 return {static_cast<u8>(x * 4), static_cast<u8>(255 - (y * 4)),
                         static_cast<u8>((x * 7 + y * 13) & 0xFF), stored(x, y)};
             });
    nlohmann::json manifest = SixWayManifest();
    manifest["frame"] = {{"width", Size / 2}, {"height", Size / 2}};
    manifest["textures"].erase("motion");
    for (const char* name : {"lightA", "lightB"})
    {
        manifest["textures"][name]["width"] = Size;
        manifest["textures"][name]["height"] = Size;
    }
    std::ofstream(dir / "blast.atlas.json") << manifest.dump(2);
    const Result<vector<u8>> blob =
        CookEntry(dir, FlipbookId, "Flipbook", "blast.atlas.json", &WindowsConfig());
    REQUIRE(blob.has_value());

    const CookedFlipbookHeader header = FlipbookHeaderOf(*blob);
    REQUIRE(header.TextureBytes[3] > 0u);
    const usize offset = SlotOffset(header, 3);
    const CookedTextureHeader texture = TextureHeaderAt(*blob, offset);
    REQUIRE(texture.Format == FormatOrdinal(Renderer::Format::BC4Unorm));
    REQUIRE(texture.Width == Size);

    // Level 0 follows the texture header, 8-byte BC4 blocks in row-major block order.
    const u8* blocks = blob->data() + offset + sizeof(CookedTextureHeader);
    i32 worst = 0;
    for (u32 by = 0; by < Size / 4; ++by)
    {
        for (u32 bx = 0; bx < Size / 4; ++bx)
        {
            std::array<u8, 16> decoded{};
            rgbcx::unpack_bc4(blocks + ((by * (Size / 4) + bx) * 8), decoded.data(), 1);
            for (u32 texel = 0; texel < 16; ++texel)
            {
                const i32 error = std::abs(static_cast<i32>(decoded[texel]) -
                                           stored(bx * 4 + texel % 4, by * 4 + texel / 4));
                worst = std::max(worst, error);
            }
        }
    }
    CHECK(worst <= TemperatureErrorBound);
}

namespace
{
    // Cooks a texture descriptor beside its image and returns the texture blob, or the error.
    Result<vector<u8>> CookTexture(const path& dir, const nlohmann::json& descriptor,
                                   const BuildConfiguration* config = nullptr)
    {
        std::ofstream(dir / "image.tex.json") << descriptor.dump(2);
        return CookEntry(dir, TextureId, "Texture", "image.tex.json", config);
    }

    // The RGBA8 texel at `index` of the uncompressed level beginning at `offset`.
    Texel Rgba8At(const vector<u8>& blob, const usize offset, const usize index)
    {
        Texel texel{};
        std::memcpy(texel.data(), blob.data() + offset + (index * 4), 4);
        return texel;
    }
}

TEST_CASE("Cooker: a premultiplied texture's mips average its colour as stored")
{
    // One opaque premultiplied texel among three transparent ones: the 1x1 level's colour is the
    // plain average, where straight-alpha filtering weights it back up to the opaque texel's.
    const path dir = CaseDir("alpha_pm");
    WriteTga(dir / "image.tga", 2, 2, [](const u32 x, const u32 y) -> Texel
             { return x == 0 && y == 0 ? Texel{200, 100, 40, 255} : Texel{0, 0, 0, 0}; });
    const nlohmann::json base = {{"image", "image.tga"}, {"srgb", false}, {"compression", "None"}};
    const usize levelOne = sizeof(CookedTextureHeader) + (2 * 2 * 4);

    nlohmann::json premultiplied = base;
    premultiplied["alpha"] = "premultiplied";
    const Result<vector<u8>> averaged = CookTexture(dir, premultiplied);
    REQUIRE(averaged.has_value());
    const Texel plain = Rgba8At(*averaged, levelOne, 0);
    CHECK(std::abs(plain[0] - 50) <= 1);
    CHECK(std::abs(plain[1] - 25) <= 1);
    CHECK(std::abs(plain[3] - 64) <= 1);

    nlohmann::json straight = base;
    straight["alpha"] = "straight";
    const Result<vector<u8>> weighted = CookTexture(dir, straight);
    REQUIRE(weighted.has_value());
    CHECK(Rgba8At(*weighted, levelOne, 0)[0] > 100);
}

TEST_CASE("Cooker: source_channel cooks one channel of the image at a Mask role")
{
    const path dir = CaseDir("source_channel");
    const std::array<u8, 4> alphas = {10, 80, 160, 250};
    WriteTga(dir / "image.tga", 2, 2,
             [&](const u32 x, const u32 y) -> Texel { return {200, 30, 90, alphas[(y * 2) + x]}; });
    const Result<vector<u8>> blob = CookTexture(dir, {{"image", "image.tga"},
                                                      {"role", "Mask"},
                                                      {"compression", "None"},
                                                      {"generate_mips", false},
                                                      {"source_channel", "a"}});
    REQUIRE(blob.has_value());
    for (usize texel = 0; texel < 4; ++texel)
    {
        CAPTURE(texel);
        const Texel stored = Rgba8At(*blob, sizeof(CookedTextureHeader), texel);
        CHECK(stored[0] == alphas[texel]);
        CHECK(stored[3] == 255);
    }
}

TEST_CASE("Cooker: the alpha and source_channel keys are refused on a float source")
{
    const path dir = CaseDir("float_keys");
    const string exr =
        (path(VENG_COOKER_TEST_FIXTURE_DIR) / "textures" / "texture_float_rgba.exr").string();
    for (const char* key : {"alpha", "source_channel"})
    {
        CAPTURE(key);
        nlohmann::json descriptor = {{"image", exr}, {"role", "Mask"}};
        descriptor[key] = string(key) == "alpha" ? "data" : "a";
        const Result<vector<u8>> blob = CookTexture(dir, descriptor);
        REQUIRE_FALSE(blob.has_value());
        CHECK(blob.error().find("image.tex.json") != string::npos);
        CHECK(blob.error().find(fmt::format("\"{}\"", key)) != string::npos);
    }
}

TEST_CASE("Cooker: a data role's BC7 keeps blue and alpha at least as well as a colour role's")
{
    // Blue and alpha carry gradients independent of each other and of red and green, as the
    // channels of a packed data texture do; equal channel weights spend no fewer bits on them.
    constexpr u32 Size = 16;
    const auto texel = [](const u32 x, const u32 y) -> Texel
    {
        return {static_cast<u8>((x * 37 + y * 91) & 0xFF),
                static_cast<u8>((x * 53 + y * 29) & 0xFF), static_cast<u8>(x * 16),
                static_cast<u8>(255 - (y * 12))};
    };
    const path dir = CaseDir("bc7_weights");
    WriteTga(dir / "image.tga", Size, Size, texel);

    const auto blueAlphaError = [&](const char* role, const bool srgb) -> i64
    {
        const Result<vector<u8>> blob = CookTexture(
            dir, {{"image", "image.tga"}, {"role", role}, {"srgb", srgb}, {"generate_mips", false}},
            &WindowsConfig());
        REQUIRE(blob.has_value());
        const u8* blocks = blob->data() + sizeof(CookedTextureHeader);
        i64 error = 0;
        for (u32 block = 0; block < (Size / 4) * (Size / 4); ++block)
        {
            std::array<bc7decomp::color_rgba, 16> decoded{};
            bc7decomp::unpack_bc7(blocks + (block * 16), decoded.data());
            for (u32 index = 0; index < 16; ++index)
            {
                const Texel source = texel(((block % (Size / 4)) * 4) + (index % 4),
                                           ((block / (Size / 4)) * 4) + (index / 4));
                error +=
                    std::abs(decoded[index].b - source[2]) + std::abs(decoded[index].a - source[3]);
            }
        }
        return error;
    };
    CHECK(blueAlphaError("Packed", false) <= blueAlphaError("Color", true));
}
