// Flipbook load: flipbook-atlas manifests synthesized here cook through the cook cache, mount, and
// LoadSync into a Flipbook exposing every texture its header carries and the shading, motion,
// emission and frame blend it records; a schema-1 sheet loads as an unlit, stepped flipbook; and a
// header whose texture slots or emission keys disagree with it is refused as corrupt.

#include <cstring>
#include <filesystem>
#include <fstream>
#include "support/TempPath.h"
#include "support/TestCook.h"

#include <doctest/doctest.h>
#include <fmt/format.h>
#include <nlohmann/json.hpp>

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/Flipbook.h>
#include <Veng/Asset/Texture.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>

#include <gpu/fixture.h>

using namespace Veng;

namespace
{
    constexpr AssetId FlipbookId{0x5B8EDF540C1E2C05ULL};

    // Writes an 8x8 uncompressed 32-bit top-left-origin TGA of one flat texel.
    void WriteTga(const path& file)
    {
        std::ofstream out(file, std::ios::binary);
        const u8 header[18] = {0, 0, 2, 0, 0, 0, 0, 0, 0, 0, 0, 0, 8, 0, 8, 0, 32, 0x28};
        out.write(reinterpret_cast<const char*>(header), sizeof(header));
        for (u32 texel = 0; texel < 8 * 8; ++texel)
        {
            const u8 bgra[4] = {200, 120, 40, 180};
            out.write(reinterpret_cast<const char*>(bgra), sizeof(bgra));
        }
    }

    // Cooks `manifest` beside its images and mounts the result into `assets`.
    void CookAndMount(AssetManager& assets, const nlohmann::json& manifest, const string& tag)
    {
        const path dir = Veng::TestSupport::TempDir() / fmt::format("veng_gpu_flipbook_{}", tag);
        std::filesystem::remove_all(dir);
        std::filesystem::create_directories(dir);
        for (const char* file : {"atlas.tga", "lightA.tga", "lightB.tga", "motion.tga"})
        {
            WriteTga(dir / file);
        }
        std::ofstream(dir / "fx.atlas.json") << manifest.dump(2);
        const nlohmann::json pack = {
            {"version", 1},
            {"assets",
             {{{"id", fmt::format("0x{:016X}", FlipbookId.Value)},
               {"type", "Flipbook"},
               {"source", "fx.atlas.json"}}}},
        };
        std::ofstream(dir / "pack.json") << pack.dump(2);

        Cook::Cooker cooker;
        Cook::RegisterBuiltinImporters(cooker);
        const path archive = dir / "fx.vengpack";
        REQUIRE(Veng::TestSupport::CookCached(cooker, dir / "pack.json", archive).has_value());
        REQUIRE(assets.Mount(archive).has_value());
    }

    nlohmann::json Entry(const char* file, const char* colorSpace)
    {
        return {{"file", file}, {"width", 8}, {"height", 8}, {"colorSpace", colorSpace}};
    }

    // Loads a hand-built flipbook blob and returns the load's error kind.
    optional<AssetError> LoadError(Test::GpuFixture& fixture, const CookedFlipbookHeader& header)
    {
        vector<u8> blob(sizeof(header));
        std::memcpy(blob.data(), &header, sizeof(header));
        AssetManager assets(fixture.Context, fixture.Tasks, fixture.Types);
        ArchiveWriter writer;
        writer.Add(FlipbookId, AssetTypes::Flipbook, blob);
        const MountHandle mount = assets.MountMemory(writer.Build(), "corrupt_flipbook");
        const AssetResult<AssetHandle<Flipbook>> result = assets.LoadSync<Flipbook>(FlipbookId);
        if (result.has_value())
        {
            return std::nullopt;
        }
        return result.error().Kind;
    }

    // A header that passes every check before the texture slots.
    CookedFlipbookHeader ValidPrefix()
    {
        CookedFlipbookHeader header{};
        header.Version = CookedFlipbookVersion;
        header.Columns = 2;
        header.Rows = 2;
        header.FrameCount = 4;
        header.FrameWidth = 4;
        header.FrameHeight = 4;
        header.Fps = 24.0f;
        return header;
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "flipbook loader: a six-way flipbook exposes its channels")
{
    nlohmann::json motion = Entry("motion.tga", "linear");
    motion["scale"] = 0.5;
    const nlohmann::json manifest = {
        {"version", 2},
        {"type", "flipbook-atlas"},
        {"frame", {{"width", 4}, {"height", 4}}},
        {"columns", 2},
        {"rows", 2},
        {"frames", 4},
        {"fps", 24},
        {"loop", true},
        {"blend", "alpha"},
        {"shading", "sixway"},
        {"textures",
         {{"lightA", Entry("lightA.tga", "srgb")},
          {"lightB", Entry("lightB.tga", "srgb")},
          {"motion", motion}}},
        {"frameScale", {0.25, 0.5, 0.75, 1.0}},
        {"emission",
         {{"ignition", 0.3},
          {"ramp",
           {{{"t", 0.3}, {"color", {1.0, 0.2, 0.0}}}, {{"t", 1.0}, {"color", {8.0, 6.0, 4.0}}}}}}},
    };

    AssetManager assets(Context, Tasks, Types);
    CookAndMount(assets, manifest, "sixway");
    const AssetResult<AssetHandle<Flipbook>> loaded = assets.LoadSync<Flipbook>(FlipbookId);
    REQUIRE(loaded.has_value());
    const Flipbook& flipbook = *loaded->Get();

    CHECK(flipbook.GetShading() == FlipbookShading::SixWay);
    for (const Ref<Texture>* texture :
         {&flipbook.GetAtlas(), &flipbook.GetLightB(), &flipbook.GetMotion()})
    {
        REQUIRE(*texture != nullptr);
        CHECK((*texture)->GetExtent() == uvec2(8, 8));
    }
    // The cook moves the temperature into a single-channel texture of its own.
    REQUIRE(flipbook.GetTemperatureTexture() != nullptr);
    CHECK(flipbook.GetTemperatureTexture() != flipbook.GetLightB());
    CHECK(flipbook.GetTemperatureChannel() == 0u);
    CHECK(flipbook.GetTemperatureEncoding() == FlipbookTemperatureEncoding::Sqrt);

    CHECK(flipbook.GetMotionScale() == doctest::Approx(0.5f));
    CHECK(flipbook.GetFrameBlend() == FlipbookFrameBlend::Motion);
    CHECK(flipbook.GetEmission().Ignition == doctest::Approx(0.3f));
    REQUIRE(flipbook.GetEmission().Ramp.size() == 2);
    CHECK(flipbook.GetEmission().Ramp[1].T == doctest::Approx(1.0f));
    CHECK(flipbook.GetEmission().Ramp[1].Color.y == doctest::Approx(6.0f));
    CHECK(flipbook.GetFrameScale(0) == doctest::Approx(0.25f));
    CHECK(flipbook.GetFrameScale(3) == doctest::Approx(1.0f));
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "flipbook loader: a schema-1 sheet loads as an unlit, stepped flipbook")
{
    const nlohmann::json manifest = {
        {"version", 1},
        {"type", "flipbook-atlas"},
        {"atlas", {{"file", "atlas.tga"}, {"width", 8}, {"height", 8}}},
        {"frame", {{"width", 4}, {"height", 4}}},
        {"columns", 2},
        {"rows", 2},
        {"frames", 3},
        {"fps", 12.5},
        {"loop", false},
        {"blend", "additive"},
        {"alpha", "coverage"},
        {"colorSpace", "srgb"},
        {"worldExtent", {2.0, 4.0, 2.0}},
        {"pivot", {1.0, 1.0, 1.0}},
    };

    AssetManager assets(Context, Tasks, Types);
    CookAndMount(assets, manifest, "v1");
    const AssetResult<AssetHandle<Flipbook>> loaded = assets.LoadSync<Flipbook>(FlipbookId);
    REQUIRE(loaded.has_value());
    const Flipbook& flipbook = *loaded->Get();

    CHECK(flipbook.GetColumns() == 2);
    CHECK(flipbook.GetRows() == 2);
    CHECK(flipbook.GetFrameSize() == uvec2(4, 4));
    CHECK(flipbook.GetClip().FrameCount == 3);
    CHECK(flipbook.GetClip().Fps == doctest::Approx(12.5f));
    CHECK_FALSE(flipbook.GetClip().Loop);
    CHECK(flipbook.GetBlend() == FlipbookBlend::Additive);
    CHECK(flipbook.GetAlphaMode() == FlipbookAlpha::Coverage);
    REQUIRE(flipbook.GetWorldExtent().has_value());
    CHECK(flipbook.GetWorldExtent()->y == doctest::Approx(4.0f));
    REQUIRE(flipbook.GetPivot().has_value());
    CHECK(flipbook.GetAnchor().y == doctest::Approx(0.25f));

    CHECK(flipbook.GetShading() == FlipbookShading::Unlit);
    REQUIRE(flipbook.GetAtlas() != nullptr);
    CHECK(flipbook.GetLightB() == nullptr);
    CHECK(flipbook.GetMotion() == nullptr);
    CHECK(flipbook.GetTemperatureTexture() == nullptr);
    CHECK(flipbook.GetFrameBlend() == FlipbookFrameBlend::Step);
    CHECK(flipbook.GetFrameScale(1) == 1.0f);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "flipbook loader: a corrupt header is refused")
{
    // A six-way header with no second lightmap.
    CookedFlipbookHeader sixWay = ValidPrefix();
    sixWay.Shading = 1;
    sixWay.TextureBytes[0] = 64;
    CHECK(LoadError(*this, sixWay) == AssetError::Corrupt);

    // Texture slots claiming more bytes than the blob holds.
    CookedFlipbookHeader overrun = ValidPrefix();
    overrun.TextureBytes[0] = 4096;
    CHECK(LoadError(*this, overrun) == AssetError::Corrupt);

    // More emission keys than a header holds.
    CookedFlipbookHeader keys = ValidPrefix();
    keys.TextureBytes[0] = 64;
    keys.EmissionKeyCount = CookedFlipbookMaxEmissionKeys + 1;
    CHECK(LoadError(*this, keys) == AssetError::Corrupt);
}
