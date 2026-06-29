// Cook-on-demand plumbing test: cooks a single texture source asset
// (brick_basecolor.tex.json) into an in-memory single-entry .vengpack via
// Cooker::CookSource — the path the editor's CookSession drives off the render
// thread — and checks the bytes parse as a valid archive carrying the target id.
// CPU-only: the texture importer never links into libveng.

#include <cstring>
#include <filesystem>

#include <doctest/doctest.h>

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Project/BuildConfiguration.h>
#include <Veng/Renderer/Types.h>

using namespace Veng;
using namespace Veng::Cook;

TEST_CASE("Cooker: CookSource produces a mountable in-memory archive for a texture")
{
    const path source = path(VENG_HT_ASSETS_DIR) / "textures" / "brick_basecolor.tex.json";
    const AssetId targetId{0xC00C0DE000000001ULL};

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    const Result<vector<u8>> bytes = cooker.CookSource(source, targetId, AssetType::Texture);
    REQUIRE(bytes.has_value());
    CHECK_FALSE(bytes->empty());

    const Result<ArchiveReader> reader = ArchiveReader::FromBytes(*bytes);
    REQUIRE(reader.has_value());

    const optional<ArchiveEntry> entry = reader->Find(targetId);
    REQUIRE(entry.has_value());
    CHECK(entry->Type == AssetType::Texture);
    CHECK_FALSE(entry->Blob.empty());
}

TEST_CASE("Cooker: CookSource threads the active configuration into role resolution")
{
    // The editor cook-on-demand path resolves a texture's compression role through the active
    // build configuration exactly as the file-based build does. texture_role.tex.json declares
    // "role": "Color"; without a configuration the cook uses the zero-config ASTC default, and
    // under the windows configuration (Color → BC7Srgb) it resolves to BC7.
    const path source = path(VENG_COOKER_TEST_FIXTURE_DIR) / "textures" / "texture_role.tex.json";
    const path configFile = path(VENG_COOKER_TEST_FIXTURE_DIR) / "windows.buildcfg";
    const AssetId targetId{0xC00C0DE000000003ULL};

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    const auto cookedFormat = [&](const BuildConfiguration* config) -> u32
    {
        const Result<vector<u8>> bytes =
            cooker.CookSource(source, targetId, AssetType::Texture, {}, nullptr, nullptr, config);
        REQUIRE(bytes.has_value());

        const Result<ArchiveReader> reader = ArchiveReader::FromBytes(*bytes);
        REQUIRE(reader.has_value());
        const optional<ArchiveEntry> entry = reader->Find(targetId);
        REQUIRE(entry.has_value());
        REQUIRE(entry->Blob.size() >= sizeof(CookedTextureHeader));

        CookedTextureHeader header{};
        std::memcpy(&header, entry->Blob.data(), sizeof(header));
        return header.Format;
    };

    SUBCASE("a null configuration uses the zero-config ASTC default")
    {
        CHECK(cookedFormat(nullptr) == static_cast<u32>(Renderer::Format::ASTC4x4Srgb));
    }

    SUBCASE("the windows configuration resolves Color to BC7Srgb")
    {
        const Result<BuildConfiguration> config = ParseBuildConfiguration(configFile);
        REQUIRE(config.has_value());
        CHECK(cookedFormat(&*config) == static_cast<u32>(Renderer::Format::BC7Srgb));
    }
}

TEST_CASE("Cooker: CookSource on a missing source reports a located error")
{
    const path missing = path(VENG_HT_ASSETS_DIR) / "textures" / "does_not_exist.tex.json";

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    const Result<vector<u8>> bytes = cooker.CookSource(missing, AssetId{0x1}, AssetType::Texture);
    CHECK_FALSE(bytes.has_value());
}

TEST_CASE("Cooker: CookSource resolves a material's shaders and textures against a reference pack")
{
    // The material's shaders and textures are named by AssetId, not inline, so the
    // cook needs the pack manifests to resolve them — the editor's cook-on-demand
    // path. The fragment shader and albedo texture live in the sample pack; the
    // standard surface vertex shader lives in the engine core pack.
    const path source = path(VENG_HT_ASSETS_DIR) / "materials" / "brick.vmat.json";
    const path manifest = path(VENG_HT_ASSETS_DIR) / "sample.vengpack.json";
    const path corePack = path(VENG_CORE_PACK_JSON);
    const AssetId targetId{0xC00C0DE000000002ULL};

    Cooker cooker;
    RegisterBuiltinImporters(cooker);

    SUBCASE("with the manifest the cross-asset references resolve")
    {
        const path refs[] = {manifest, corePack};
        // The brick fragment shader `#include`s the engine header; the cook needs the engine
        // core shader dir on its Slang search path to resolve the cross-pack include.
        const Result<vector<u8>> bytes =
            cooker.CookSource(source, targetId, AssetType::Material, refs, nullptr, nullptr,
                              nullptr, path(VENG_CORE_SHADER_DIR));
        REQUIRE(bytes.has_value());

        const Result<ArchiveReader> reader = ArchiveReader::FromBytes(*bytes);
        REQUIRE(reader.has_value());

        const optional<ArchiveEntry> entry = reader->Find(targetId);
        REQUIRE(entry.has_value());
        CHECK(entry->Type == AssetType::Material);
        CHECK_FALSE(entry->Blob.empty());
    }

    SUBCASE("without it the unresolved shader is a located error")
    {
        const Result<vector<u8>> bytes = cooker.CookSource(source, targetId, AssetType::Material);
        CHECK_FALSE(bytes.has_value());
    }
}
