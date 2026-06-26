// Compression-role resolution test: cooks a fixture texture under (a) no configuration, (b) a BC7
// configuration, and (c) a raw "compression" override, asserting the written CookedTextureHeader
// Format ordinal each way; checks the default-role-from-srgb guess; and round-trips a *.buildcfg
// JSON fixture through the cooker's hand-parser.

#include <cstring>
#include <filesystem>

#include <doctest/doctest.h>

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Project/BuildConfiguration.h>
#include <Veng/Project/CompressionFormat.h>
#include <Veng/Renderer/Types.h>

using namespace Veng;
using namespace Veng::Cook;

namespace
{
    // Cooks a single-texture pack and returns the written texture's Format ordinal.
    u32 CookFormat(const path& packJson, const AssetId id, const BuildConfiguration* config,
                   const path& configFile)
    {
        const path outArchive =
            std::filesystem::temp_directory_path() / "veng_cooker_role_resolution.vengpack";

        Cooker cooker;
        RegisterBuiltinImporters(cooker);

        const VoidResult cookResult = cooker.CookPack(packJson, outArchive, {}, nullptr, nullptr,
                                                      nullptr, config, configFile);
        REQUIRE(cookResult.has_value());

        const Result<ArchiveReader> reader = ArchiveReader::Open(outArchive);
        REQUIRE(reader.has_value());

        const optional<ArchiveEntry> entry = reader->Find(id);
        REQUIRE(entry.has_value());
        REQUIRE(entry->Blob.size() >= sizeof(CookedTextureHeader));

        CookedTextureHeader header{};
        std::memcpy(&header, entry->Blob.data(), sizeof(header));

        std::filesystem::remove(outArchive);
        return header.Format;
    }
}

TEST_CASE("Cooker: a role resolves to the zero-config ASTC default with no configuration")
{
    const path fixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
    // texture_role.tex.json is "role": "Color", "srgb": true — no configuration, so the importer
    // falls back to the hardcoded ASTC default, sRGB-aware: ASTC4x4Srgb (ordinal 24).
    const u32 format = CookFormat(fixtureDir / "texture_role_pack.json",
                                  AssetId{0x87D4E44F6ED405CEULL}, nullptr, {});
    CHECK(format == static_cast<u32>(Renderer::Format::ASTC4x4Srgb));
}

TEST_CASE("Cooker: a Color role resolves to the configuration's BC7 format")
{
    const path fixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
    const Result<BuildConfiguration> config =
        ParseBuildConfiguration(fixtureDir / "windows.buildcfg");
    REQUIRE(config.has_value());

    // The windows configuration maps Color → BC7Srgb (ordinal 22); the Color-role texture resolves
    // through it, the role carrying the sRGB intent.
    const u32 format =
        CookFormat(fixtureDir / "texture_role_pack.json", AssetId{0x87D4E44F6ED405CEULL}, &*config,
                   fixtureDir / "windows.buildcfg");
    CHECK(format == static_cast<u32>(Renderer::Format::BC7Srgb));
}

TEST_CASE("Cooker: a raw 'compression' override wins over the role and the configuration")
{
    const path fixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
    const Result<BuildConfiguration> config =
        ParseBuildConfiguration(fixtureDir / "windows.buildcfg");
    REQUIRE(config.has_value());

    // texture_override.tex.json is "role": "Color", "srgb": false, "compression": "bc7": the raw
    // codec pins BC7 directly, keyed off srgb=false → BC7Unorm (21), regardless of the BC7Srgb the
    // configuration's Color role would resolve to.
    const u32 format =
        CookFormat(fixtureDir / "texture_override_pack.json", AssetId{0x2B99AE9EF36284E1ULL},
                   &*config, fixtureDir / "windows.buildcfg");
    CHECK(format == static_cast<u32>(Renderer::Format::BC7Unorm));
}

TEST_CASE("Cooker: an absent role defaults from the srgb flag to Color under a configuration")
{
    const path fixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
    const Result<BuildConfiguration> config =
        ParseBuildConfiguration(fixtureDir / "windows.buildcfg");
    REQUIRE(config.has_value());

    // texture_norole_srgb.tex.json has "srgb": true and no role: the importer guesses Color, which
    // the configuration maps to BC7Srgb (22).
    const u32 format =
        CookFormat(fixtureDir / "texture_norole_pack.json", AssetId{0x605F613507654CD5ULL},
                   &*config, fixtureDir / "windows.buildcfg");
    CHECK(format == static_cast<u32>(Renderer::Format::BC7Srgb));
}

TEST_CASE("Cooker: ParseBuildConfiguration round-trips the buildcfg JSON name tables")
{
    const path fixtureDir = path(VENG_COOKER_TEST_FIXTURE_DIR);
    const Result<BuildConfiguration> config =
        ParseBuildConfiguration(fixtureDir / "windows.buildcfg");
    REQUIRE(config.has_value());

    CHECK(config->Name == "windows");
    CHECK(config->Target == "windows-x64");
    CHECK(config->OutputSuffix == "-windows");
    CHECK(config->CompressionLevel == 12);

    // The role table's enums parse by name, never ordinal.
    CHECK(config->Formats.Color == CompressionFormat::BC7Srgb);
    CHECK(config->Formats.Normal == CompressionFormat::BC7Unorm);
    CHECK(config->Formats.Mask == CompressionFormat::BC7Unorm);
    CHECK(config->Formats.HDR == CompressionFormat::RGBA16Sfloat);
    CHECK(config->Formats.UI == CompressionFormat::BC7Unorm);
}
