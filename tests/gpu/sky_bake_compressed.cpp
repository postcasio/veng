// A sky bake into a block-compressed (BC6H) cube. A pattern material bakes into a BC6H cube and into
// a float cube of the same face, and the compressed cube is read back through a sampling pass and
// held to the float one: a smooth sky within BC6H's error bound, isolated bright texels kept as their
// blocks' brightest with their blocks' light, per-texel speckle within stated luminance and chroma
// bounds, the lighting reduction equal to the float source's box reduction, the mip chain's means,
// and a cached cube restoring the same texels.
//
// Gated on GPU_GBUFFER_FIXTURE_DIR (the in-process cook of the pattern material); every case skips
// on a device without BC block compression.

#include <array>
#include <atomic>
#include <cmath>

#include <doctest/doctest.h>

#include <gpu/fixture.h>

#ifdef GPU_GBUFFER_FIXTURE_DIR

#include <filesystem>
#include "support/TempPath.h"
#include "support/TestCook.h"

#include <fmt/format.h>
#include <glm/gtc/packing.hpp>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Math/SphericalHarmonics.h>
#include <Veng/Persistence/DerivedDataCache.h>
#include <Veng/Renderer/BakedSkyCube.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/GeneratedTextureService.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/TextureReadback.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Task/TaskSystem.h>

#include "Renderer/EnvironmentIbl.h"

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr AssetId PatternInstanceId{0x0560B81C3F365974ULL};

    // The pattern material's modes (sky_bc6h_pattern.frag).
    constexpr f32 SmoothPattern = 0.0f;
    constexpr f32 PointPattern = 1.0f;
    constexpr f32 SpecklePattern = 2.0f;

    AssetHandle<MaterialInstance> CookAndLoad(AssetManager& assets)
    {
        const path fixtureDir = path(GPU_GBUFFER_FIXTURE_DIR);
        const path outArchive =
            Veng::TestSupport::TempDir() / "veng_gpu_sky_bake_compressed.vengpack";
        Cook::Cooker cooker;
        Cook::RegisterBuiltinImporters(cooker);
        const VoidResult cooked = Veng::TestSupport::CookCached(
            cooker, fixtureDir / "gbuffer_pack.json", outArchive, {}, nullptr, nullptr, nullptr,
            nullptr, {}, path(VENG_CORE_SHADER_DIR));
        REQUIRE(cooked.has_value());
        REQUIRE(assets.Mount(outArchive).has_value());
        const AssetResult<AssetHandle<MaterialInstance>> pattern =
            assets.LoadSync<MaterialInstance>(PatternInstanceId);
        REQUIRE(pattern.has_value());
        return *pattern;
    }

    // Requests a one-layer bake of the pattern, pumps it to completion and lands it.
    void BakePattern(Context& context, BakedSkyCube& cube, MaterialInstance& pattern,
                     const f32 mode, const u32 encodeTilePixels = 0)
    {
        pattern.SetParam("Mode", vec4(mode, static_cast<f32>(cube.GetFaceSize()), 0.0f, 0.0f));
        const std::array<BakedSkyCube::SkyBakeLayer, 1> layers{
            {{.Material = &pattern, .Blend = BlendState::Opaque()}}};
        GeneratedTextureService& service = context.GetGeneratedTextures();
        service.SetCostBudget(GeneratedTextureService::UnlimitedCostBudget);
        cube.RequestBake(service, layers, {}, {}, encodeTilePixels);
        for (u32 frame = 0; frame < 256 && cube.IsBakePending(); ++frame)
        {
            context.BeginFrame();
            context.EndFrame();
        }
        REQUIRE_FALSE(cube.IsBakePending());
        context.ImmediateCommands([&](CommandBuffer& cmd) { CHECK(cube.RecordAmortized(cmd)); });
        service.ClearCostBudget();
    }

    vector<vec4> Read(Context& context, AssetManager& assets, const Ref<Image>& image,
                      const u32 mip = 0)
    {
        Result<TexelReadback> readback = ReadbackTexels(context, assets, image, mip);
        REQUIRE(readback.has_value());
        return std::move(readback->Texels);
    }

    f32 Luminance(const vec4& c)
    {
        return (0.2126f * c.r) + (0.7152f * c.g) + (0.0722f * c.b);
    }

    vec3 MeanRgb(const vector<vec4>& texels)
    {
        glm::dvec3 sum(0.0);
        for (const vec4& texel : texels)
        {
            sum += glm::dvec3(vec3(texel));
        }
        return vec3(sum / static_cast<f64>(texels.size()));
    }

    // The largest per-channel relative gap between two readbacks of one shape.
    f32 WorstRelativeGap(const vector<vec4>& actual, const vector<vec4>& expected)
    {
        f32 worst = 0.0f;
        for (usize i = 0; i < actual.size(); ++i)
        {
            const vec3 gap = glm::abs(vec3(actual[i]) - vec3(expected[i])) / vec3(expected[i]);
            worst = std::max({worst, gap.x, gap.y, gap.z});
        }
        return worst;
    }

    // The irradiance SH of a lighting level, projected as the SH readback does from its halves.
    Sh9 ProjectLevel(const vector<vec4>& texels, const u32 faceSize)
    {
        vector<u8> halves(texels.size() * 8);
        auto* out = reinterpret_cast<u16*>(halves.data());
        for (usize i = 0; i < texels.size(); ++i)
        {
            for (u32 c = 0; c < 4; ++c)
            {
                out[(i * 4) + c] = glm::packHalf1x16(texels[i][static_cast<i32>(c)]);
            }
        }
        return EnvironmentIbl::ProjectCubeToIrradianceSh(halves, faceSize);
    }

    struct TempCacheRoot
    {
        path Dir;

        TempCacheRoot()
        {
            static std::atomic<u64> counter{0};
            Dir = TestSupport::TempDir() /
                  fmt::format("sky-bc6h-{}", counter.fetch_add(1, std::memory_order_relaxed));
            std::filesystem::remove_all(Dir);
        }

        ~TempCacheRoot() { std::filesystem::remove_all(Dir); }
    };

    // A bake runs, and its cache store lands, through the task pump and the frame loop together.
    void DriveWithTasks(Context& context, TaskSystem& tasks, const function<bool()>& done)
    {
        for (u32 frame = 0; frame < 256 && !done(); ++frame)
        {
            tasks.PumpMainThread();
            context.BeginFrame();
            context.EndFrame();
        }
        tasks.PumpMainThread();
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "sky bake compressed: a smooth sky round-trips BC6H within its error bound")
{
    if (!Context.IsBlockCompressionSupported())
    {
        MESSAGE("textureCompressionBC unsupported on this device; skipping");
        return;
    }
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const AssetHandle<MaterialInstance> pattern = CookAndLoad(assets);
    const Ref<DescriptorSetLayout> layout = BakedSkyCube::CreateConsumerSetLayout(Context);

    // Four tiles a face side, so tile edges cross the face.
    constexpr u32 FaceSize = 64;
    const Unique<BakedSkyCube> compressed = BakedSkyCube::Create(
        Context, layout, Format::RGBA16Sfloat, FaceSize, &assets, Format::BC6HUfloat);
    const Unique<BakedSkyCube> source =
        BakedSkyCube::Create(Context, layout, Format::RGBA16Sfloat, FaceSize, &assets);
    REQUIRE(compressed->IsCompressed());
    REQUIRE(compressed->GetCubeImage()->GetFormat() == Format::BC6HUfloat);
    BakePattern(Context, *compressed, *pattern.Get(), SmoothPattern, 16);
    BakePattern(Context, *source, *pattern.Get(), SmoothPattern);

    // The one-region mode keeps 10 of an endpoint's 15 half-float bits, a step of 2^-5 of its
    // magnitude, so a smooth block's texels land within a few percent of their source — channels
    // running opposite ways included, which a block's bounding-box diagonal alone cannot hold.
    const vector<vec4> decoded = Read(Context, assets, compressed->GetCubeImage());
    const vector<vec4> expected = Read(Context, assets, source->GetCubeImage());
    REQUIRE(decoded.size() == expected.size());
    CHECK(WorstRelativeGap(decoded, expected) < 0.04f);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "sky bake compressed: isolated bright texels and speckle survive BC6H")
{
    if (!Context.IsBlockCompressionSupported())
    {
        MESSAGE("textureCompressionBC unsupported on this device; skipping");
        return;
    }
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const AssetHandle<MaterialInstance> pattern = CookAndLoad(assets);
    const Ref<DescriptorSetLayout> layout = BakedSkyCube::CreateConsumerSetLayout(Context);

    constexpr u32 FaceSize = 64;
    const Unique<BakedSkyCube> compressed = BakedSkyCube::Create(
        Context, layout, Format::RGBA16Sfloat, FaceSize, &assets, Format::BC6HUfloat);
    const Unique<BakedSkyCube> source =
        BakedSkyCube::Create(Context, layout, Format::RGBA16Sfloat, FaceSize, &assets);

    // A dark field holding at most one bright texel per block: each block keeps its light, and its
    // bright texel stays its brightest.
    BakePattern(Context, *compressed, *pattern.Get(), PointPattern);
    BakePattern(Context, *source, *pattern.Get(), PointPattern);
    {
        const vector<vec4> decoded = Read(Context, assets, compressed->GetCubeImage());
        const vector<vec4> expected = Read(Context, assets, source->GetCubeImage());
        REQUIRE(decoded.size() == expected.size());
        constexpr u32 Blocks = FaceSize / 4;
        f32 worstSum = 0.0f;
        u32 points = 0;
        u32 pointsKept = 0;
        for (u32 face = 0; face < BakedSkyCube::CubeFaces; ++face)
        {
            for (u32 block = 0; block < Blocks * Blocks; ++block)
            {
                vec3 decodedSum(0.0f);
                vec3 expectedSum(0.0f);
                f32 brightest = -1.0f;
                usize brightestDecoded = 0;
                optional<usize> point;
                for (u32 t = 0; t < 16; ++t)
                {
                    const u32 x = ((block % Blocks) * 4) + (t % 4);
                    const u32 y = ((block / Blocks) * 4) + (t / 4);
                    const usize i = ((static_cast<usize>(face) * FaceSize + y) * FaceSize) + x;
                    decodedSum += vec3(decoded[i]);
                    expectedSum += vec3(expected[i]);
                    if (Luminance(decoded[i]) > brightest)
                    {
                        brightest = Luminance(decoded[i]);
                        brightestDecoded = i;
                    }
                    if (Luminance(expected[i]) > 1.0f)
                    {
                        point = i;
                    }
                }
                const vec3 gap = glm::abs(decodedSum - expectedSum) / expectedSum;
                worstSum = std::max({worstSum, gap.x, gap.y, gap.z});
                if (point.has_value())
                {
                    ++points;
                    pointsKept += brightestDecoded == *point ? 1 : 0;
                }
            }
        }
        REQUIRE(points > 0);
        CHECK(pointsKept == points);
        CHECK(worstSum < 0.05f);
    }

    // Independent per-texel speckle over two decades of luminance, with a ±10% spread of chroma.
    BakePattern(Context, *compressed, *pattern.Get(), SpecklePattern);
    BakePattern(Context, *source, *pattern.Get(), SpecklePattern);
    {
        const vector<vec4> decoded = Read(Context, assets, compressed->GetCubeImage());
        const vector<vec4> expected = Read(Context, assets, source->GetCubeImage());
        f32 worstLuminance = 0.0f;
        f64 meanLuminance = 0.0;
        f32 worstChroma = 0.0f;
        for (usize i = 0; i < decoded.size(); ++i)
        {
            const f32 sourceLuminance = Luminance(expected[i]);
            const f32 decodedLuminance = Luminance(decoded[i]);
            const f32 luminanceGap = std::abs(decodedLuminance - sourceLuminance) / sourceLuminance;
            worstLuminance = std::max(worstLuminance, luminanceGap);
            meanLuminance += luminanceGap;
            const vec3 chromaGap = glm::abs((vec3(decoded[i]) / decodedLuminance) -
                                            (vec3(expected[i]) / sourceLuminance));
            worstChroma = std::max({worstChroma, chromaGap.x, chromaGap.y, chromaGap.z});
        }
        meanLuminance /= static_cast<f64>(decoded.size());
        // Two decades over a block's sixteen indices is a step of 100^(1/15) in the half-float
        // space the decoder interpolates in, so a texel's ideal error is up to half that, about
        // 17%: the mean sits well under it, and no texel is off by half its value. A block holds one
        // line of colours, so independent chroma flattens toward the block's own: no texel is
        // further off than the pattern's spread allows.
        CHECK(meanLuminance < 0.08);
        CHECK(worstLuminance < 0.5f);
        CHECK(worstChroma < 0.2f);
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "sky bake compressed: the mip chain and lighting reduction are box reductions of "
                  "the source")
{
    if (!Context.IsBlockCompressionSupported())
    {
        MESSAGE("textureCompressionBC unsupported on this device; skipping");
        return;
    }
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const AssetHandle<MaterialInstance> pattern = CookAndLoad(assets);
    const Ref<DescriptorSetLayout> layout = BakedSkyCube::CreateConsumerSetLayout(Context);

    // A face past the lighting reduction's size, and tiles small enough that the coarse face mips
    // (8 down to 1) are encoded from the reduction, two of them partial blocks.
    constexpr u32 FaceSize = 512;
    constexpr u32 FullChain = 10;
    const Unique<BakedSkyCube> compressed = BakedSkyCube::Create(
        Context, layout, Format::RGBA16Sfloat, FaceSize, &assets, Format::BC6HUfloat);
    const Unique<BakedSkyCube> source =
        BakedSkyCube::Create(Context, layout, Format::RGBA16Sfloat, FaceSize, &assets);
    BakePattern(Context, *compressed, *pattern.Get(), SmoothPattern, 128);
    BakePattern(Context, *source, *pattern.Get(), SmoothPattern);

    // Both display cubes carry a full chain. The float cube lights from its own mips at the
    // reduction's size; the compressed one from a float reduction beside it.
    CHECK(compressed->GetCubeImage()->GetMipLevels() == FullChain);
    CHECK(source->GetCubeImage()->GetMipLevels() == FullChain);
    CHECK(source->GetLightingImage() == source->GetCubeImage());
    CHECK(source->GetLightingBaseMip() == 1);
    CHECK(compressed->GetLightingImage() != compressed->GetCubeImage());
    CHECK(compressed->GetLightingFaceSize() == BakedSkyCube::LightingFaceSize);
    CHECK(source->GetLightingFaceSize() == BakedSkyCube::LightingFaceSize);

    // The compressed cube's lighting reduction is the float source's box reduction, texel for
    // texel, within half-float rounding.
    const vector<vec4> reduction = Read(Context, assets, compressed->GetLightingImage());
    const vector<vec4> sourceReduction =
        Read(Context, assets, source->GetLightingImage(), source->GetLightingBaseMip());
    REQUIRE(reduction.size() == sourceReduction.size());
    CHECK(WorstRelativeGap(reduction, sourceReduction) < 2e-3f);

    // A box chain keeps the mean, so every mip's mean is the source's: a tile-encoded mip, a mip
    // encoded from the reduction, the one-texel mip, and the float chain's own.
    const vec3 sourceMean = MeanRgb(Read(Context, assets, source->GetCubeImage()));
    f32 worstMean = 0.0f;
    for (const u32 mip : {3u, 7u, 9u})
    {
        const vec3 mean = MeanRgb(Read(Context, assets, compressed->GetCubeImage(), mip));
        const vec3 gap = glm::abs(mean - sourceMean) / sourceMean;
        worstMean = std::max({worstMean, gap.x, gap.y, gap.z});
    }
    CHECK(worstMean < 0.02f);
    const vec3 floatCoarse = MeanRgb(Read(Context, assets, source->GetCubeImage(), 9));
    const vec3 floatGap = glm::abs(floatCoarse - sourceMean) / sourceMean;
    CHECK(std::max({floatGap.x, floatGap.y, floatGap.z}) < 2e-3f);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "sky bake compressed: a cached cube restores its chain and lighting reduction")
{
    if (!Context.IsBlockCompressionSupported())
    {
        MESSAGE("textureCompressionBC unsupported on this device; skipping");
        return;
    }
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const AssetHandle<MaterialInstance> pattern = CookAndLoad(assets);
    const Ref<DescriptorSetLayout> layout = BakedSkyCube::CreateConsumerSetLayout(Context);

    const TempCacheRoot root;
    const Result<Unique<DerivedDataCache>> cache =
        DerivedDataCache::Open({.Root = root.Dir, .Generation = "gpu-case"});
    REQUIRE(cache.has_value());
    GeneratedTextureService& service = Context.GetGeneratedTextures();
    service.SetCache(cache->get(), &Tasks);
    service.SetCostBudget(GeneratedTextureService::UnlimitedCostBudget);

    constexpr u32 FaceSize = 64;
    constexpr string_view CacheKey = "sky-bc6h/speckle";
    pattern.Get()->SetParam("Mode", vec4(SpecklePattern, static_cast<f32>(FaceSize), 0.0f, 0.0f));
    const std::array<BakedSkyCube::SkyBakeLayer, 1> layers{
        {{.Material = pattern.Get(), .Blend = BlendState::Opaque()}}};

    // A fresh bake renders its tiles and stores the chain and the reduction.
    const Unique<BakedSkyCube> fresh = BakedSkyCube::Create(Context, layout, Format::RGBA16Sfloat,
                                                            FaceSize, &assets, Format::BC6HUfloat);
    fresh->RequestBake(service, layers, string(CacheKey));
    DriveWithTasks(Context, Tasks, [&] { return !fresh->IsBakePending(); });
    REQUIRE_FALSE(fresh->IsBakePending());
    Context.ImmediateCommands([&](CommandBuffer& cmd) { CHECK(fresh->RecordAmortized(cmd)); });
    CHECK(fresh->GetFaceRendersRecorded() > 0);
    DriveWithTasks(Context, Tasks, [&] { return (*cache)->Contains(string(CacheKey)); });
    REQUIRE((*cache)->Contains(string(CacheKey)));

    // A second cube under the same key restores it without rendering a tile.
    const Unique<BakedSkyCube> restored = BakedSkyCube::Create(
        Context, layout, Format::RGBA16Sfloat, FaceSize, &assets, Format::BC6HUfloat);
    restored->RequestBake(service, layers, string(CacheKey));
    DriveWithTasks(Context, Tasks, [&] { return !restored->IsBakePending(); });
    REQUIRE_FALSE(restored->IsBakePending());
    Context.ImmediateCommands([&](CommandBuffer& cmd) { CHECK(restored->RecordAmortized(cmd)); });
    CHECK(restored->GetFaceRendersRecorded() == 0);

    bool sameChain = true;
    for (u32 mip = 0; mip < fresh->GetCubeImage()->GetMipLevels(); ++mip)
    {
        sameChain = sameChain && Read(Context, assets, fresh->GetCubeImage(), mip) ==
                                     Read(Context, assets, restored->GetCubeImage(), mip);
    }
    CHECK(sameChain);
    const vector<vec4> freshLighting =
        Read(Context, assets, fresh->GetLightingImage(), fresh->GetShReadbackMipLevel());
    const vector<vec4> restoredLighting =
        Read(Context, assets, restored->GetLightingImage(), restored->GetShReadbackMipLevel());
    CHECK(freshLighting == restoredLighting);
    const Sh9 freshSh = ProjectLevel(freshLighting, fresh->GetShReadbackFaceSize());
    const Sh9 restoredSh = ProjectLevel(restoredLighting, restored->GetShReadbackFaceSize());
    CHECK(freshSh.Coefficients == restoredSh.Coefficients);

    service.ClearCostBudget();
    service.SetCache(nullptr, nullptr);
}

#endif
