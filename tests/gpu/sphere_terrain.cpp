// Sphere terrain's tile pool against a live device, with the test generator (a few sinusoids of
// the direction, tests/shaders/sphere_terrain_test_generator.comp.slang):
//   1. The pinned top becomes resident within a bounded number of frames, and its measured bounds
//      bracket the texels the generator wrote.
//   2. Two adjacent tiles of one level agree on the texels they share along an edge — within a
//      face also in each other's border — to the generator's gradient times the f32 direction
//      error their two anchors carry.
//   3. The device evaluates the tile mapping to the precision the host twin holds: deep tiles on a
//      sphere thousands of kilometres across are placed to the millimetre.
//   4. MorphError is zero where a tile and its parent carry no detail, and not where they differ.
//   5. A renderer drawing a scene's terrain selects a covering set and fills the pool through it.
//
// Skips cleanly (exit 77) on a machine with no Vulkan ICD, like the rest of the gpu band.

#include <algorithm>
#include <cmath>
#include <cstring>
#include <limits>
#include <numbers>

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Math/CubeSphere.h>
#include <Veng/Path.h>
#include <Veng/Renderer/AsyncReadback.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/ComputePipeline.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/GeneratedTextureService.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/SphereTerrain.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr AssetId TestGeneratorCompId{0x2B127C3FF19DC81DULL};
    constexpr AssetId MappingProbeCompId{0x73076F8C5B6B5499ULL};

    constexpr f64 Radius = 6.4e6;
    constexpr f32 Amplitude = 100.0f;
    constexpr f32 Frequency = 3.0f;
    constexpr u32 Texels = SphereTerrainTileTexels;

    // Mirrors GeneratorParams in the test generator.
    struct GeneratorParams
    {
        f32 Amplitude = 0.0f;
        f32 Frequency = 0.0f;
        u32 FlatFromLevel = 0;
        u32 Pad = 0;
    };

    DescriptorBinding Binding(const u32 binding, const DescriptorType type)
    {
        return {.Binding = binding, .Type = type, .Count = 1, .Stages = ShaderStage::Compute};
    }

    class TestGenerator final : public SphereTerrainGenerator
    {
    public:
        TestGenerator(Context& context, AssetManager& assets, const u32 flatFromLevel)
            : m_Context(context)
        {
            const AssetResult<AssetHandle<Shader>> shader =
                assets.LoadSync<Shader>(TestGeneratorCompId);
            REQUIRE_MESSAGE(shader.has_value(), "load the sphere terrain test generator");
            m_SetLayout = DescriptorSetLayout::Create(
                context, {.Name = "Test Generator Set Layout",
                          .Bindings = {Binding(0, DescriptorType::StorageImage),
                                       Binding(1, DescriptorType::StorageBuffer)}});
            const Ref<PipelineLayout> layout = PipelineLayout::Create(
                context, {.Name = "Test Generator Layout",
                          .DescriptorSetLayouts = {m_SetLayout},
                          .PushConstantRanges = {PushConstantRange::Of<SphereTerrainTileConstants>(
                              ShaderStage::Compute)}});
            m_Pipeline = ComputePipeline::Create(
                context,
                {.Name = "Test Generator",
                 .PipelineLayout = layout,
                 .ShaderStage = {.Stage = ShaderStage::Compute, .Module = shader->Get()->Module}});
            m_Params = Buffer::Create(context, {.Name = "Test Generator Params",
                                                .Size = sizeof(GeneratorParams),
                                                .Usage = BufferUsage::Storage,
                                                .HostMapped = true});
            const GeneratorParams params{
                .Amplitude = Amplitude, .Frequency = Frequency, .FlatFromLevel = flatFromLevel};
            std::memcpy(m_Params->GetMappedData(), &params, sizeof(params));
        }

        vector<SphereTerrainChannelInfo> GetChannels() const override
        {
            return {{.Name = "Height", .TexelFormat = Format::R32Sfloat}};
        }

        void RecordTile(CommandBuffer& cmd, const SphereTerrainTileWork& work) override
        {
            // One set per tile, held for the case's lifetime: a set is never rewritten while a
            // submitted frame may still read it.
            const Ref<DescriptorSet> set = DescriptorSet::Create(
                m_Context, {.Name = "Test Generator Set", .Layout = m_SetLayout});
            set->Write(0, work.ChannelViews[0]);
            set->Write(1, m_Params);
            m_Sets.push_back(set);
            cmd.BindPipeline(m_Pipeline);
            cmd.BindDescriptorSets({set.get()}, BindlessRegistry::FirstUserSet,
                                   PipelineBindPoint::Compute);
            cmd.PushConstants(work.Constants);
            cmd.Dispatch((Texels + 7) / 8, (Texels + 7) / 8, 1);
            Tiles++;
        }

        f32 GetLevelHeightBound(const u32 level) const override
        {
            (void)level;
            return 3.0f * Amplitude;
        }

        u32 Tiles = 0;

    private:
        Context& m_Context;
        Ref<DescriptorSetLayout> m_SetLayout;
        Ref<ComputePipeline> m_Pipeline;
        Ref<Buffer> m_Params;
        vector<Ref<DescriptorSet>> m_Sets;
    };

    Ref<SphereTerrainSource> MakeSource(Context& context, AssetManager& assets,
                                        const u32 flatFromLevel = 99, const u32 capacity = 192)
    {
        return SphereTerrainSource::Create(
            context, assets,
            {
                .Name = "Test Sphere",
                .Generator = CreateRef<TestGenerator>(context, assets, flatFromLevel),
                .Radius = Radius,
                .MinHeight = -3.0f * Amplitude,
                .MaxHeight = 3.0f * Amplitude,
                .MaxResidentTiles = capacity,
            });
    }

    void DriveFrame(Context& context)
    {
        context.BeginFrame();
        context.EndFrame();
    }

    template <typename Done>
    u32 DriveUntil(Context& context, Done done, const u32 limit)
    {
        u32 frames = 0;
        while (!done() && frames < limit)
        {
            DriveFrame(context);
            frames++;
        }
        return frames;
    }

    // Requests a tile's whole ancestry each frame until it is resident; a request under a parent
    // still generating waits and is made again the next frame.
    bool MakeResident(Context& context, SphereTerrainSource& source,
                      const vector<CubeSphereTileKey>& tiles)
    {
        const auto allResident = [&]
        {
            return std::ranges::all_of(tiles, [&](const CubeSphereTileKey key)
                                       { return source.IsResident(key); });
        };
        for (u32 frame = 0; frame < 64 && !allResident(); frame++)
        {
            for (const CubeSphereTileKey key : tiles)
            {
                for (CubeSphereTileKey up = key; up.GetLevel() > source.GetPinnedLevel();
                     up = up.GetParent())
                {
                    source.Request(up, 1000);
                }
            }
            DriveFrame(context);
        }
        return allResident();
    }

    // Reads one tile's height layer back, row-major.
    vector<f32> ReadLayer(Context& context, const SphereTerrainSource& source, const u32 layer)
    {
        vector<f32> heights;
        bool delivered = false;
        context.GetAsyncReadback().Request({
            .Name = "Tile Read",
            .Image = source.GetChannelImage(0),
            .ArrayLayer = layer,
            .OnComplete =
                [&](const std::span<const u8> bytes)
            {
                heights.resize(bytes.size() / sizeof(f32));
                std::memcpy(heights.data(), bytes.data(), bytes.size());
                delivered = true;
            },
        });
        DriveUntil(context, [&] { return delivered; }, 16);
        REQUIRE(delivered);
        REQUIRE(heights.size() == Texels * Texels);
        return heights;
    }

    f32 At(const vector<f32>& heights, const u32 x, const u32 y)
    {
        return heights[(y * Texels) + x];
    }

    dvec3 TexelDirection(const CubeSphereTileKey key, const u32 x, const u32 y)
    {
        const auto parameter = [](const u32 k)
        {
            return static_cast<f64>(static_cast<i32>(k) -
                                    static_cast<i32>(SphereTerrainTileBorder)) /
                   SphereTerrainTileCells;
        };
        return CubeSphereTileDirection(GetCubeSphereTileGeometry(key), parameter(x), parameter(y));
    }

    // The agreement two tiles' copies of one texel owe each other: the generator's gradient (three
    // unit wave vectors) times the f32 rounding of a unit direction, plus the device sine's own
    // rounding at the generator's amplitude.
    constexpr f64 AgreementBound =
        (3.0 * Amplitude * Frequency * 4.2e-7) + (3.0 * Amplitude * 1.0e-6);
}

TEST_CASE_FIXTURE(Test::GpuFixture,
                  "sphere terrain: the pinned top generates and its bounds bracket its texels")
{
    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(path(TEST_SHADER_PACK)).has_value());
    Context.GetGeneratedTextures().SetCostBudget(GeneratedTextureService::UnlimitedCostBudget);

    const Ref<SphereTerrainSource> source = MakeSource(Context, assets);
    const u32 frames = DriveUntil(Context, [&] { return source->IsPinnedTopResident(); }, 32);
    CHECK(source->IsPinnedTopResident());
    // One frame per pinned level once the pool is allocated, a level waiting on its parent.
    CHECK(frames <= 2 * (source->GetPinnedLevel() + 1) + 2);

    const CubeSphereTileKey probe = CubeSphereTileKey::Make(CubeSphere::Face::NegY, 2, 1, 2);
    DriveUntil(
        Context, [&] { return source->FindResident(probe)->Measured; },
        Context.GetMaxFramesInFlight() + 4);
    const optional<SphereTerrainResidentTile> tile = source->FindResident(probe);
    REQUIRE(tile.has_value());
    REQUIRE(tile->Measured);

    const vector<f32> heights = ReadLayer(Context, *source, tile->Layer);
    f32 low = std::numeric_limits<f32>::max();
    f32 high = std::numeric_limits<f32>::lowest();
    for (u32 y = SphereTerrainTileBorder; y <= SphereTerrainTileBorder + SphereTerrainTileCells;
         y++)
    {
        for (u32 x = SphereTerrainTileBorder; x <= SphereTerrainTileBorder + SphereTerrainTileCells;
             x++)
        {
            low = std::min(low, At(heights, x, y));
            high = std::max(high, At(heights, x, y));
        }
    }
    CHECK(tile->MinHeight == low);
    CHECK(tile->MaxHeight == high);
    CHECK(high > low);
    CHECK(tile->MorphError > 0.0f);

    const SphereTerrainSourceStats stats = source->GetStats();
    CHECK(stats.PinnedTiles == 126u);
    CHECK(stats.Capacity == 192u);
    CHECK(stats.Bytes == u64{192} * Texels * Texels * sizeof(f32));
}

TEST_CASE_FIXTURE(Test::GpuFixture,
                  "sphere terrain: adjacent tiles of one level agree on their edge")
{
    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(path(TEST_SHADER_PACK)).has_value());
    Context.GetGeneratedTextures().SetCostBudget(GeneratedTextureService::UnlimitedCostBudget);
    const Ref<SphereTerrainSource> source = MakeSource(Context, assets);
    DriveUntil(Context, [&] { return source->IsPinnedTopResident(); }, 32);

    // Within a face: x = 10 and 11. Across a face edge: the last column of +X and its neighbour.
    const CubeSphereTileKey left = CubeSphereTileKey::Make(CubeSphere::Face::PosX, 5, 10, 12);
    const CubeSphereTileKey right = CubeSphereTileKey::Make(CubeSphere::Face::PosX, 5, 11, 12);
    const CubeSphereTileKey edge = CubeSphereTileKey::Make(CubeSphere::Face::PosX, 5, 31, 12);
    const CubeSphereTileKey across = edge.GetNeighbour(CubeSphere::Edge::PosU);
    REQUIRE(across.GetFace() != CubeSphere::Face::PosX);
    REQUIRE(MakeResident(Context, *source, {left, right, edge, across}));

    const vector<f32> leftHeights = ReadLayer(Context, *source, source->FindResident(left)->Layer);
    const vector<f32> rightHeights =
        ReadLayer(Context, *source, source->FindResident(right)->Layer);
    f64 withinFace = 0.0;
    for (u32 y = 0; y < Texels; y++)
    {
        // The left tile's columns 128..132 are the right tile's 0..4: the shared column at s = 1,
        // and two texels of border on either side.
        for (u32 k = 0; k <= 2 * SphereTerrainTileBorder; k++)
        {
            withinFace =
                std::max(withinFace,
                         static_cast<f64>(std::abs(At(leftHeights, SphereTerrainTileCells + k, y) -
                                                   At(rightHeights, k, y))));
        }
    }

    // Across the face edge the shared row's texels are found by direction: the other face's tile
    // runs along the edge in its own orientation.
    const vector<f32> edgeHeights = ReadLayer(Context, *source, source->FindResident(edge)->Layer);
    const vector<f32> acrossHeights =
        ReadLayer(Context, *source, source->FindResident(across)->Layer);
    const u32 first = SphereTerrainTileBorder;
    const u32 last = SphereTerrainTileBorder + SphereTerrainTileCells;
    f64 acrossFace = 0.0;
    f64 worstMatch = 0.0;
    for (u32 y = first; y <= last; y++)
    {
        const dvec3 direction = TexelDirection(edge, last, y);
        f64 best = std::numeric_limits<f64>::max();
        f32 match = 0.0f;
        for (u32 i = first; i <= last; i++)
        {
            for (const uvec2 texel :
                 {uvec2(first, i), uvec2(last, i), uvec2(i, first), uvec2(i, last)})
            {
                const f64 gap = glm::length(TexelDirection(across, texel.x, texel.y) - direction);
                if (gap < best)
                {
                    best = gap;
                    match = At(acrossHeights, texel.x, texel.y);
                }
            }
        }
        worstMatch = std::max(worstMatch, best);
        acrossFace =
            std::max(acrossFace, static_cast<f64>(std::abs(At(edgeHeights, last, y) - match)));
    }
    MESSAGE("edge agreement within a face ", withinFace, " m, across a face ", acrossFace,
            " m, bound ", AgreementBound, " m");
    CHECK(worstMatch < 1e-12);
    CHECK(withinFace <= AgreementBound);
    CHECK(acrossFace <= AgreementBound);
}

TEST_CASE_FIXTURE(Test::GpuFixture,
                  "sphere terrain: the device places a deep texel to the millimetre")
{
    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(path(TEST_SHADER_PACK)).has_value());
    const AssetResult<AssetHandle<Shader>> shader = assets.LoadSync<Shader>(MappingProbeCompId);
    REQUIRE(shader.has_value());

    struct ProbeSample
    {
        u32 Tile = 0;
        f32 S = 0.0f;
        f32 T = 0.0f;
        u32 Pad = 0;
    };
    vector<CubeSphereTileKey> keys;
    vector<SphereTerrainTileConstants> tiles;
    for (u32 level = 20; level <= 24; level++)
    {
        const u32 n = 1u << level;
        const CubeSphereTileKey key = CubeSphereTileKey::Make(
            static_cast<CubeSphere::Face>(level % 6), level, (n / 7) * 5, n - 3);
        keys.push_back(key);
        tiles.push_back(PackSphereTerrainTileConstants(key, Radius));
    }
    vector<ProbeSample> samples;
    for (u32 tile = 0; tile < tiles.size(); tile++)
    {
        for (const f32 s : {0.0f, 0.3f, 1.0f, -0.015625f})
        {
            for (const f32 t : {0.0f, 0.71f, 1.0f})
            {
                samples.push_back({.Tile = tile, .S = s, .T = t});
            }
        }
    }

    const auto makeBuffer = [&](const char* name, const usize bytes)
    {
        return Buffer::Create(
            Context,
            {.Name = name, .Size = bytes, .Usage = BufferUsage::Storage, .HostMapped = true});
    };
    const Ref<Buffer> tileBuffer = makeBuffer("Probe Tiles", tiles.size() * sizeof(tiles[0]));
    const Ref<Buffer> sampleBuffer =
        makeBuffer("Probe Samples", samples.size() * sizeof(samples[0]));
    const Ref<Buffer> resultBuffer = makeBuffer("Probe Results", samples.size() * sizeof(vec4));
    std::memcpy(tileBuffer->GetMappedData(), tiles.data(), tiles.size() * sizeof(tiles[0]));
    std::memcpy(sampleBuffer->GetMappedData(), samples.data(), samples.size() * sizeof(samples[0]));

    const Ref<DescriptorSetLayout> setLayout = DescriptorSetLayout::Create(
        Context, {.Name = "Probe Set Layout",
                  .Bindings = {Binding(0, DescriptorType::StorageBuffer),
                               Binding(1, DescriptorType::StorageBuffer),
                               Binding(2, DescriptorType::StorageBuffer)}});
    const Ref<PipelineLayout> layout = PipelineLayout::Create(
        Context, {.Name = "Probe Layout",
                  .DescriptorSetLayouts = {setLayout},
                  .PushConstantRanges = {PushConstantRange::Of<u32>(ShaderStage::Compute)}});
    const Ref<ComputePipeline> pipeline = ComputePipeline::Create(
        Context, {.Name = "Probe",
                  .PipelineLayout = layout,
                  .ShaderStage = {.Stage = ShaderStage::Compute, .Module = shader->Get()->Module}});
    const Ref<DescriptorSet> set =
        DescriptorSet::Create(Context, {.Name = "Probe Set", .Layout = setLayout});
    set->Write(0, tileBuffer);
    set->Write(1, sampleBuffer);
    set->Write(2, resultBuffer);
    const u32 count = static_cast<u32>(samples.size());
    Context.ImmediateCommands(
        [&](CommandBuffer& cmd)
        {
            cmd.BindPipeline(pipeline);
            cmd.BindDescriptorSets({set.get()}, BindlessRegistry::FirstUserSet,
                                   PipelineBindPoint::Compute);
            cmd.PushConstants(count);
            cmd.Dispatch((count + 63) / 64, 1, 1);
        });

    vector<vec4> results(samples.size());
    std::memcpy(results.data(), resultBuffer->GetMappedData(), results.size() * sizeof(vec4));
    f64 worstPlacement = 0.0;
    f64 worstFraction = 0.0;
    for (usize i = 0; i < samples.size(); i++)
    {
        const CubeSphereTileGeometry geometry = GetCubeSphereTileGeometry(keys[samples[i].Tile]);
        const dvec3 reference = CubeSphereTileDirection(geometry, samples[i].S, samples[i].T);
        const dvec3 placed = geometry.Center + dvec3(vec3(results[i]));
        worstPlacement = std::max(worstPlacement, glm::length(placed - reference) * Radius);
        const f64 fraction =
            CubeSphere::TileFraction(samples[i].S, geometry.ThetaStartU, geometry.ThetaSpan);
        worstFraction = std::max(worstFraction, std::abs(fraction - results[i].w));
    }
    MESSAGE("device placement worst ", worstPlacement * 1000.0, " mm, fraction worst ",
            worstFraction);
    CHECK(worstPlacement < 1e-3);
    CHECK(worstFraction < 1e-6);
}

TEST_CASE_FIXTURE(Test::GpuFixture,
                  "sphere terrain: MorphError is zero where a tile and its parent carry no detail")
{
    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(path(TEST_SHADER_PACK)).has_value());
    Context.GetGeneratedTextures().SetCostBudget(GeneratedTextureService::UnlimitedCostBudget);
    // Detail through level 2; flat from level 3.
    const Ref<SphereTerrainSource> source = MakeSource(Context, assets, 3);
    DriveUntil(Context, [&] { return source->IsPinnedTopResident(); }, 32);

    const CubeSphereTileKey deep = CubeSphereTileKey::Make(CubeSphere::Face::PosZ, 4, 6, 9);
    const CubeSphereTileKey flat = deep.GetParent();
    REQUIRE(MakeResident(Context, *source, {deep}));
    DriveUntil(
        Context, [&] { return source->FindResident(deep)->Measured; },
        Context.GetMaxFramesInFlight() + 4);

    const optional<SphereTerrainResidentTile> deepTile = source->FindResident(deep);
    const optional<SphereTerrainResidentTile> flatTile = source->FindResident(flat);
    REQUIRE(deepTile.has_value());
    REQUIRE(flatTile.has_value());
    REQUIRE(deepTile->Measured);
    REQUIRE(flatTile->Measured);
    CHECK(deepTile->MinHeight == 0.0f);
    CHECK(deepTile->MaxHeight == 0.0f);
    CHECK(deepTile->MorphError == 0.0f);
    // The first flat level departs from its detailed parent by no more than the parent's range.
    const optional<SphereTerrainResidentTile> detailed = source->FindResident(flat.GetParent());
    REQUIRE(detailed.has_value());
    CHECK(flatTile->MorphError > 0.0f);
    CHECK(flatTile->MorphError <=
          std::max(std::abs(detailed->MinHeight), std::abs(detailed->MaxHeight)));
}

TEST_CASE_FIXTURE(Test::GpuFixture,
                  "sphere terrain: a renderer selects a covering set and fills the pool through it")
{
    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(path(TEST_SHADER_PACK)).has_value());
    Context.GetGeneratedTextures().SetCostBudget(GeneratedTextureService::UnlimitedCostBudget);
    const Ref<SphereTerrainSource> source = MakeSource(Context, assets, 99, 512);

    // The eye stands 10 km up; the scene origin rides it, so the camera is at the origin.
    RegisterBuiltinTypes(Types);
    const Unique<Scene> world = Scene::Create(Types);
    Scene& scene = *world;
    const dvec3 eye = glm::normalize(dvec3(0.3, 0.9, 0.2)) * (Radius + 1.0e4);
    scene.Add<FloatingOrigin>(scene.CreateEntity()).Position = eye;
    const Entity sphere = scene.CreateEntity();
    scene.Add<SphereTerrain>(sphere).Source = source;

    CameraView camera;
    camera.SetPerspective(glm::radians(60.0f), 1.0f, 0.1f, 1000.0f);
    camera.SetView(vec3(0.0f), vec3(glm::normalize(-eye)), vec3(1.0f, 0.0f, 0.0f));

    const Unique<SceneRenderer> renderer = SceneRenderer::Create({
        .Context = Context,
        .Assets = assets,
        .OutputFormat = Format::RGBA16Sfloat,
        .Extent = {64, 64},
        .Settings = SceneRendererSettings{.Mode = DebugView::Albedo, .Occlusion = false},
    });
    for (u32 frame = 0; frame < 40; frame++)
    {
        CommandBuffer& cmd = Context.BeginFrame();
        renderer->Execute(cmd,
                          Renderer::SceneView{.World = scene, .Camera = camera, .Delta = 0.0f});
        Context.EndFrame();
    }

    const vector<SphereTerrainDrawnTile> drawn = renderer->GetSphereTerrainSelection(sphere);
    REQUIRE_FALSE(drawn.empty());
    f64 solidAngle = 0.0;
    u32 deepest = 0;
    u32 badMorph = 0;
    for (const SphereTerrainDrawnTile& tile : drawn)
    {
        const CubeSphereTileGeometry g = GetCubeSphereTileGeometry(tile.Key);
        const auto triangle = [](const dvec3& a, const dvec3& b, const dvec3& c)
        {
            return 2.0 * std::atan2(std::abs(glm::dot(a, glm::cross(b, c))),
                                    1.0 + glm::dot(a, b) + glm::dot(b, c) + glm::dot(c, a));
        };
        solidAngle += triangle(g.Corners[0], g.Corners[1], g.Corners[3]) +
                      triangle(g.Corners[0], g.Corners[3], g.Corners[2]);
        deepest = std::max(deepest, tile.Key.GetLevel());
        badMorph += tile.MorphStart < tile.MorphEnd && tile.MorphStart > 0.0f ? 0u : 1u;
    }
    CHECK(std::abs(solidAngle - (4.0 * std::numbers::pi)) < 1e-6);
    CHECK(deepest > source->GetPinnedLevel() + 3);
    CHECK(badMorph == 0u);
    CHECK(source->GetStats().ResidentTiles > 126u);
    CHECK(renderer->GetSphereTerrainStats().DrawnTiles == drawn.size());
}
