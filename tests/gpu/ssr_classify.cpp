// The stochastic reflection chain's front: tile classification, the work lists, the dispatch records
// and the ray directions. Every case renders one frame of a synthetic scene at 64² — a white floor of
// one roughness, seen straight down so it fills the frame or obliquely and rolled so its edges cut
// quads and tiles — and reads the chain's lists back:
//
//  - each density traces the rays per quad it promises, a mirror traces every pixel, and a floor
//    past the roughness cutoff traces none and lists no tile;
//  - every glossy pixel is traced or named by its quad's base ray exactly once, and no background
//    pixel is either;
//  - the tile list names every tile holding a glossy pixel once, and only those;
//  - the dispatch records match the totals, and a dispatch through them visits each ray once;
//  - the GGX lobe sampler draws the distribution of visible normals it implements;
//  - the blue-noise tile is uniform per channel and spreads evenly over every 8x8 window.
//
// The materials are forward_opaque's untextured parameter block, so the g-buffer holds exactly the
// authored roughness. Skips cleanly (exit 77) on a machine with no Vulkan ICD.

#include <algorithm>
#include <array>
#include <cmath>
#include <cstring>
#include <numbers>
#include <set>

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Primitives.h>
#include <Veng/Asset/RawAsset.h>
#include <Veng/Asset/Shader.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/ComputePipeline.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/LightPacking.h>
#include <Veng/Renderer/PipelineLayout.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include <gpu/fixture.h>

#include "support/TempPath.h"
#include "support/TestCook.h"

// glm's packing header after Veng.h, which configures glm first.
#include <glm/gtc/packing.hpp>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // forward_lighting_pack's opaque material: BaseColor, Roughness, Metallic, Occlusion, untextured.
    constexpr AssetId OpaqueMaterial{0x3AEA87F2E94072A6ULL};
    // The test shader pack's ray visitor and lobe sampler.
    constexpr AssetId SsrRayVisitCompId{0xA3338D14AAC995BCULL};
    constexpr AssetId SsrLobeSampleCompId{0x6356EE0051FD4F95ULL};
    // The core pack's blue-noise tile.
    constexpr AssetId SsrBlueNoiseId{0xCDED5EB51013870BULL};

    constexpr uvec2 Extent{64, 64};
    constexpr u32 Pixels = Extent.x * Extent.y;
    constexpr u32 Quads = Pixels / 4;

    // The perceptual roughness cutoff every case renders under: GGX alpha 0.25.
    constexpr f32 MaxRoughness = 0.5f;

    // The counter slots and the dispatch records (Veng/ssr_ray_list.slang).
    constexpr u32 RayCount = 1;
    constexpr u32 TileCount = 3;

    // Cooks and mounts the untextured material pack, returning the archive the caller removes.
    path MountPack(AssetManager& assets, const char* archiveName)
    {
        const path fixtureDir = path(GPU_GBUFFER_FIXTURE_DIR);
        const path outArchive = Veng::TestSupport::TempDir() / archiveName;
        Cook::Cooker cooker;
        Cook::RegisterBuiltinImporters(cooker);
        REQUIRE(Veng::TestSupport::CookCached(cooker, fixtureDir / "forward_lighting_pack.json",
                                              outArchive, {}, nullptr, nullptr, nullptr, nullptr,
                                              {}, path(VENG_CORE_SHADER_DIR))
                    .has_value());
        REQUIRE(assets.Mount(outArchive).has_value());
        return outArchive;
    }

    AssetHandle<MaterialInstance> MakeMaterial(Context& context, AssetManager& assets,
                                               const f32 roughness)
    {
        const AssetResult<AssetHandle<Material>> parent = assets.LoadSync<Material>(OpaqueMaterial);
        REQUIRE(parent.has_value());
        const AssetHandle<MaterialInstance> instance = assets.BuildSync<MaterialInstance>(
            MaterialInstanceInfo{.Name = "Floor", .Context = &context, .Parent = *parent});
        REQUIRE(instance.IsLoaded());
        auto& mat = const_cast<MaterialInstance&>(*instance.Get());
        mat.SetParam("BaseColor", vec4(1.0f));
        mat.SetParam("Roughness", roughness);
        mat.SetParam("Metallic", 0.0f);
        return instance;
    }

    // How the camera sees the floor.
    enum class Framing : u8
    {
        // Straight down onto a floor wider than the view: every pixel is floor.
        Filled,
        // Obliquely and rolled onto a small floor: its edges cut quads and tiles at a slant.
        Edged,
    };

    struct FloorScene
    {
        Unique<Scene> World;
        Ref<Mesh> Floor;
        CameraView Camera;
    };

    FloorScene BuildFloor(Context& context, AssetManager& assets, TypeRegistry& types,
                          const f32 roughness, const Framing framing)
    {
        FloorScene built{.World = Scene::Create(types)};
        Scene& scene = *built.World;
        const f32 size = framing == Framing::Filled ? 400.0f : 3.0f;
        built.Floor = Mesh::BuildSync(
            context,
            Primitives::Plane(vec2(size), uvec2(1), MakeMaterial(context, assets, roughness)),
            "Floor");
        const Entity floor = scene.CreateEntity();
        scene.Add<Transform>(floor);
        scene.Add<MeshRenderer>(floor).Mesh = assets.Adopt(built.Floor);

        const Entity light = scene.CreateEntity();
        scene.Add<Light>(light) = Light{
            .Type = LightType::Directional,
            .Direction = glm::normalize(vec3(0.2f, -1.0f, -0.3f)),
            .Color = vec3(1.0f),
            .Intensity = 4.0f / LuminousAnchor,
        };

        built.Camera.SetPerspective(glm::radians(60.0f), 1.0f, 0.1f, 1000.0f);
        if (framing == Framing::Filled)
        {
            built.Camera.SetView(vec3(0.0f, 2.0f, 0.0f), vec3(0.0f), vec3(0.0f, 0.0f, -1.0f));
        }
        else
        {
            built.Camera.SetView(vec3(0.0f, 1.5f, 2.5f), vec3(0.0f),
                                 glm::normalize(vec3(0.3f, 1.0f, 0.0f)));
        }
        return built;
    }

    SceneRendererSettings StochasticSettings(const SceneRendererSettings::SsrSamplesPerQuad density)
    {
        SceneRendererSettings settings{
            .Mode = DebugView::Final,
            .Bloom = false,
            .Shadows = false,
            .SSR = true,
            .AO = false,
        };
        settings.AntiAliasing = AntiAliasingMode::None;
        settings.SsrMethod = SceneRendererSettings::SsrTechnique::Stochastic;
        settings.SsrDensity = density;
        return settings;
    }

    template <typename T>
    vector<T> DownloadAs(const Ref<Buffer>& buffer)
    {
        const vector<u8> bytes = buffer->Download();
        vector<T> out(bytes.size() / sizeof(T));
        std::memcpy(out.data(), bytes.data(), out.size() * sizeof(T));
        return out;
    }

    // One frame's classification, read back.
    struct Classification
    {
        vector<u32> Rays;
        vector<u32> Tiles;
        vector<u32> Counters;
        vector<u32> Arguments;
        // Whether each pixel shows the floor: the lit output is non-zero exactly there.
        vector<bool> Surface;
        // Each pixel's extracted GGX alpha, in [0, 1].
        vector<f32> Alpha;
        // Whether every texel of the radiance target is zero.
        bool RadianceCleared = false;
    };

    Classification RenderOnce(const Context& context, SceneRenderer& renderer,
                              const FloorScene& scene)
    {
        context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                renderer.Execute(cmd, Renderer::SceneView{.World = *scene.World,
                                                          .Camera = scene.Camera,
                                                          .Delta = 0.0f,
                                                          .SsrMaxRoughness = MaxRoughness});
            });
        const SsrTraceTargets targets = renderer.GetSsrTraceTargets();
        REQUIRE(targets.RayList != nullptr);

        Classification result;
        result.Counters = DownloadAs<u32>(targets.Counters);
        result.Arguments = DownloadAs<u32>(targets.Arguments);
        result.Rays = DownloadAs<u32>(targets.RayList);
        result.Rays.resize(result.Counters[RayCount]);
        result.Tiles = DownloadAs<u32>(targets.TileList);
        result.Tiles.resize(result.Counters[TileCount]);

        const vector<u8> output = renderer.GetOutput()->GetImage()->Download();
        const auto* halves = reinterpret_cast<const u16*>(output.data());
        result.Surface.resize(Pixels);
        for (u32 i = 0; i < Pixels; ++i)
        {
            const f32 sum = glm::unpackHalf1x16(halves[i * 4 + 0]) +
                            glm::unpackHalf1x16(halves[i * 4 + 1]) +
                            glm::unpackHalf1x16(halves[i * 4 + 2]);
            result.Surface[i] = sum > 1e-3f;
        }

        const vector<u8> alpha = targets.Alpha->GetImage()->Download();
        result.Alpha.resize(Pixels);
        for (u32 i = 0; i < Pixels; ++i)
        {
            result.Alpha[i] = static_cast<f32>(alpha[i]) / 255.0f;
        }

        const vector<u8> radiance = targets.Radiance->GetImage()->Download();
        result.RadianceCleared =
            std::ranges::all_of(radiance, [](const u8 byte) { return byte == 0; });
        return result;
    }

    struct UnpackedRay
    {
        uvec2 Pixel;
        bool CopyHorizontal;
        bool CopyVertical;
        bool CopyDiagonal;
    };

    UnpackedRay Unpack(const u32 packed)
    {
        return {.Pixel = uvec2(packed & 32767u, (packed >> 15) & 16383u),
                .CopyHorizontal = ((packed >> 29) & 1u) != 0,
                .CopyVertical = ((packed >> 30) & 1u) != 0,
                .CopyDiagonal = ((packed >> 31) & 1u) != 0};
    }

    u32 Index(const uvec2 pixel)
    {
        return pixel.y * Extent.x + pixel.x;
    }

    Ref<DescriptorSetLayout> StorageSetLayout(Context& context, const u32 count)
    {
        vector<DescriptorBinding> bindings;
        for (u32 binding = 0; binding < count; ++binding)
        {
            bindings.push_back({.Binding = binding,
                                .Type = DescriptorType::StorageBuffer,
                                .Count = 1,
                                .Stages = ShaderStage::Compute});
        }
        return DescriptorSetLayout::Create(context,
                                           {.Name = "SSR Classify Test Set", .Bindings = bindings});
    }

    Ref<ComputePipeline> MakePipeline(Context& context, const AssetHandle<Shader>& shader,
                                      const Ref<DescriptorSetLayout>& setLayout,
                                      const u32 pushBytes)
    {
        const auto layout = PipelineLayout::Create(
            context, {.Name = "SSR Classify Test Layout",
                      .DescriptorSetLayouts = {setLayout},
                      .PushConstantRanges = {
                          {.Stages = ShaderStage::Compute, .Offset = 0, .Size = pushBytes}}});
        return ComputePipeline::Create(
            context, {.Name = "SSR Classify Test Pipeline",
                      .PipelineLayout = layout,
                      .ShaderStage = {.Stage = ShaderStage::Compute, .Module = shader->Module}});
    }

    struct RayVisitPush
    {
        u32 Width;
    };

    struct LobeSamplePush
    {
        vec4 ViewRay;
        vec4 Normal;
        f32 Alpha;
        u32 Side;
    };

    // The GGX distribution of normals visible from `toEye` (local frame, normal +Z), integrated on a
    // grid over the NDF's own cumulative parameterisation: tan²θ = α²ξ/(1 − ξ) carries D(m)·cosθ dω
    // to dξ dφ/2π, so the visible distribution G1(V)·max(0, V·m)·D(m)/V.z weighs each cell by
    // G1(V)·max(0, V·m)/(V.z·cosθ). That weight grows as 1/√(1 − ξ) toward grazing microfacets, so
    // the grid runs over s with ξ = 1 − s², whose Jacobian 2s cancels it. Returns the mean of
    // dot(L, mirror) over it and the share of L below the surface, L being toEye reflected about m.
    struct LobeReference
    {
        f64 MeanMirrorCosine;
        f64 BelowSurface;
    };

    LobeReference IntegrateVisibleNormals(const glm::dvec3& toEye, const f64 alpha)
    {
        const f64 cosView = toEye.z;
        const f64 tan2View = (1.0 - cosView * cosView) / (cosView * cosView);
        const f64 g1 = 2.0 / (1.0 + std::sqrt(1.0 + alpha * alpha * tan2View));
        const glm::dvec3 mirror(-toEye.x, -toEye.y, toEye.z);
        constexpr u32 SSteps = 256;
        constexpr u32 PhiSteps = 128;
        f64 weightSum = 0.0;
        f64 cosineSum = 0.0;
        f64 belowSum = 0.0;
        for (u32 i = 0; i < SSteps; ++i)
        {
            const f64 sCell = (i + 0.5) / SSteps;
            const f64 xi = 1.0 - sCell * sCell;
            const f64 tanTheta = alpha * std::sqrt(xi / (1.0 - xi));
            const f64 cosTheta = 1.0 / std::sqrt(1.0 + tanTheta * tanTheta);
            const f64 sinTheta = tanTheta * cosTheta;
            for (u32 j = 0; j < PhiSteps; ++j)
            {
                const f64 phi = 2.0 * std::numbers::pi * (j + 0.5) / PhiSteps;
                const glm::dvec3 m(sinTheta * std::cos(phi), sinTheta * std::sin(phi), cosTheta);
                const f64 visible = glm::dot(toEye, m);
                if (visible <= 0.0)
                {
                    continue;
                }
                const f64 weight = 2.0 * sCell * g1 * visible / (cosView * cosTheta);
                const glm::dvec3 reflected = 2.0 * visible * m - toEye;
                weightSum += weight;
                cosineSum += weight * glm::dot(reflected, mirror);
                belowSum += reflected.z < 0.0 ? weight : 0.0;
            }
        }
        return {.MeanMirrorCosine = cosineSum / weightSum, .BelowSurface = belowSum / weightSum};
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "ssr classify: each density traces its rays per quad over a glossy floor")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const path archive = MountPack(assets, "veng_gpu_ssr_classify_density.vengpack");
    const FloorScene scene = BuildFloor(Context, assets, Types, 0.3f, Framing::Filled);

    using Density = SceneRendererSettings::SsrSamplesPerQuad;
    const Unique<SceneRenderer> renderer = SceneRenderer::Create({
        .Context = Context,
        .Assets = assets,
        .OutputFormat = Context.GetOutputFormat(),
        .Extent = Extent,
        .Settings = StochasticSettings(Density::One),
    });

    for (const auto& [density, perQuad] :
         std::array{std::pair{Density::One, 1u}, std::pair{Density::Two, 2u},
                    std::pair{Density::Four, 4u}})
    {
        CAPTURE(perQuad);
        renderer->Configure(StochasticSettings(density));
        const Classification result = RenderOnce(Context, *renderer, scene);
        REQUIRE(std::ranges::all_of(result.Surface, [](const bool surface) { return surface; }));

        CHECK(result.Counters[RayCount] == Quads * perQuad);
        // The append slots start the next frame at zero.
        CHECK(result.Counters[0] == 0);
        CHECK(result.Counters[2] == 0);
        CHECK(result.Counters[TileCount] == (Extent.x / 8) * (Extent.y / 8));

        // The floor's extracted alpha is its roughness squared, to the R8 target's step.
        f32 alphaError = 0.0f;
        for (const f32 alpha : result.Alpha)
        {
            alphaError = std::max(alphaError, std::abs(alpha - 0.3f * 0.3f));
        }
        CHECK(alphaError <= 2.0f / 255.0f);
        CHECK(result.RadianceCleared);
    }

    std::filesystem::remove(archive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "ssr classify: a floor past the cutoff traces nothing, and a mirror traces all")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const path archive = MountPack(assets, "veng_gpu_ssr_classify_cutoff.vengpack");

    using Density = SceneRendererSettings::SsrSamplesPerQuad;
    const Unique<SceneRenderer> renderer = SceneRenderer::Create({
        .Context = Context,
        .Assets = assets,
        .OutputFormat = Context.GetOutputFormat(),
        .Extent = Extent,
        .Settings = StochasticSettings(Density::One),
    });

    // Above the cutoff: no ray, and no tile for the denoiser.
    {
        const FloorScene rough = BuildFloor(Context, assets, Types, 0.7f, Framing::Filled);
        const Classification result = RenderOnce(Context, *renderer, rough);
        CHECK(result.Counters[RayCount] == 0);
        CHECK(result.Counters[TileCount] == 0);
    }

    // A mirror is not reconstructed from its quad, so every pixel traces at every density.
    const FloorScene mirror = BuildFloor(Context, assets, Types, 0.0f, Framing::Filled);
    for (const Density density : {Density::One, Density::Two, Density::Four})
    {
        CAPTURE(static_cast<int>(density));
        renderer->Configure(StochasticSettings(density));
        const Classification result = RenderOnce(Context, *renderer, mirror);
        CHECK(result.Counters[RayCount] == Pixels);
        CHECK(result.Counters[TileCount] == (Extent.x / 8) * (Extent.y / 8));
        // A mirror needs no copies: it traces every pixel itself.
        CHECK(std::ranges::none_of(result.Rays,
                                   [](const u32 packed)
                                   {
                                       const UnpackedRay ray = Unpack(packed);
                                       return ray.CopyHorizontal || ray.CopyVertical ||
                                              ray.CopyDiagonal;
                                   }));
    }

    std::filesystem::remove(archive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "ssr classify: every glossy pixel is traced or copied once, and so is no other")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const path archive = MountPack(assets, "veng_gpu_ssr_classify_cover.vengpack");
    const FloorScene scene = BuildFloor(Context, assets, Types, 0.3f, Framing::Edged);

    const Unique<SceneRenderer> renderer = SceneRenderer::Create({
        .Context = Context,
        .Assets = assets,
        .OutputFormat = Context.GetOutputFormat(),
        .Extent = Extent,
        .Settings = StochasticSettings(SceneRendererSettings::SsrSamplesPerQuad::One),
    });
    const Classification result = RenderOnce(Context, *renderer, scene);

    // The framing leaves both floor and background on screen, and quads holding both.
    const u32 surfaceCount = static_cast<u32>(std::ranges::count(result.Surface, true));
    REQUIRE(surfaceCount > Pixels / 8);
    REQUIRE(surfaceCount < Pixels * 7 / 8);

    vector<u32> covered(Pixels, 0);
    for (const u32 packed : result.Rays)
    {
        const UnpackedRay ray = Unpack(packed);
        REQUIRE(ray.Pixel.x < Extent.x);
        REQUIRE(ray.Pixel.y < Extent.y);
        ++covered[Index(ray.Pixel)];
        if (ray.CopyHorizontal)
        {
            ++covered[Index(uvec2(ray.Pixel.x ^ 1u, ray.Pixel.y))];
        }
        if (ray.CopyVertical)
        {
            ++covered[Index(uvec2(ray.Pixel.x, ray.Pixel.y ^ 1u))];
        }
        if (ray.CopyDiagonal)
        {
            ++covered[Index(ray.Pixel ^ uvec2(1u))];
        }
    }
    u32 floorMiscovered = 0;
    u32 backgroundCovered = 0;
    for (u32 i = 0; i < Pixels; ++i)
    {
        if (result.Surface[i])
        {
            floorMiscovered += covered[i] == 1 ? 0 : 1;
        }
        else
        {
            backgroundCovered += covered[i] == 0 ? 0 : 1;
        }
    }
    CHECK(floorMiscovered == 0);
    CHECK(backgroundCovered == 0);
    // At one ray per quad a partial quad still traces only through its base pixel or a copy, so
    // the ray count stays under the quads holding floor.
    CHECK(result.Rays.size() <= Quads);

    // The tile list names each tile holding floor once, and no other tile.
    std::set<u32> listed;
    u32 duplicates = 0;
    for (const u32 packed : result.Tiles)
    {
        const uvec2 origin(packed & 0xFFFFu, packed >> 16);
        CHECK(origin.x % 8 == 0);
        CHECK(origin.y % 8 == 0);
        duplicates += listed.insert((origin.y / 8) * (Extent.x / 8) + origin.x / 8).second ? 0 : 1;
    }
    CHECK(duplicates == 0);
    std::set<u32> holdingFloor;
    for (u32 i = 0; i < Pixels; ++i)
    {
        if (result.Surface[i])
        {
            holdingFloor.insert((i / Extent.x / 8) * (Extent.x / 8) + (i % Extent.x) / 8);
        }
    }
    CHECK(listed == holdingFloor);

    std::filesystem::remove(archive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "ssr classify: a dispatch through the written records visits each ray once")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const path archive = MountPack(assets, "veng_gpu_ssr_classify_dispatch.vengpack");
    REQUIRE(assets.Mount(path(TEST_SHADER_PACK)).has_value());
    const FloorScene scene = BuildFloor(Context, assets, Types, 0.3f, Framing::Edged);

    const Unique<SceneRenderer> renderer = SceneRenderer::Create({
        .Context = Context,
        .Assets = assets,
        .OutputFormat = Context.GetOutputFormat(),
        .Extent = Extent,
        .Settings = StochasticSettings(SceneRendererSettings::SsrSamplesPerQuad::Two),
    });
    const Classification result = RenderOnce(Context, *renderer, scene);
    const u32 rays = result.Counters[RayCount];
    REQUIRE(rays > 0);
    CHECK(result.Arguments ==
          vector<u32>{(rays + 63) / 64, 1, 1, result.Counters[TileCount], 1, 1});

    const auto visitShader = assets.LoadSync<Shader>(SsrRayVisitCompId);
    REQUIRE_MESSAGE(visitShader, visitShader.error().Detail);
    const Ref<DescriptorSetLayout> setLayout = StorageSetLayout(Context, 3);
    const Ref<ComputePipeline> pipeline =
        MakePipeline(Context, *visitShader, setLayout, sizeof(RayVisitPush));

    const SsrTraceTargets targets = renderer->GetSsrTraceTargets();
    const vector<u32> zeros(Pixels, 0);
    const Ref<Buffer> visits =
        Buffer::Create(Context, {.Name = "SSR Ray Visits",
                                 .Size = zeros.size() * sizeof(u32),
                                 .Usage = BufferUsage::Storage | BufferUsage::TransferDst |
                                          BufferUsage::TransferSrc});
    visits->UploadSync(
        std::span(reinterpret_cast<const u8*>(zeros.data()), zeros.size() * sizeof(u32)));
    const Ref<DescriptorSet> set =
        DescriptorSet::Create(Context, {.Name = "SSR Ray Visit Set", .Layout = setLayout});
    set->Write(0, targets.RayList);
    set->Write(1, targets.Counters);
    set->Write(2, visits);

    Context.ImmediateCommands(
        [&](CommandBuffer& cmd)
        {
            RenderGraph graph(Context);
            const ResourceId rayListId = graph.ImportBuffer("Ray List");
            const ResourceId countersId = graph.ImportBuffer("Counters");
            const ResourceId argumentsId = graph.ImportBuffer("Arguments");
            const ResourceId visitsId = graph.ImportBuffer("Visits");

            // The renderer's graph wrote the lists and records in an earlier submission; a pass
            // declaring their write takes the barrier that orders this graph's reads after it.
            graph.AddComputePass("Order After Classify")
                .StorageBufferWrite(rayListId)
                .StorageBufferWrite(countersId)
                .StorageBufferWrite(argumentsId)
                .Execute([](PassContext&) {});
            graph.AddComputePass("Visit Rays")
                .StorageBufferRead(rayListId)
                .StorageBufferRead(countersId)
                .IndirectRead(argumentsId)
                .StorageBufferWrite(visitsId)
                .Execute(
                    [&](PassContext& ctx)
                    {
                        ctx.Cmd().BindPipeline(pipeline);
                        ctx.Cmd().BindDescriptorSets(
                            {.Sets = {set},
                             .FirstSet = 3,
                             .PipelineBindPoint = PipelineBindPoint::Compute});
                        ctx.Cmd().PushConstants(RayVisitPush{.Width = Extent.x});
                        ctx.Cmd().DispatchIndirect(ctx.ResolvedBuffer(argumentsId), 0);
                    });

            auto compiled = graph.Compile();
            compiled->Execute(
                cmd, std::array{
                         RenderGraph::ImportBinding{.Id = rayListId, .Buffer = targets.RayList},
                         RenderGraph::ImportBinding{.Id = countersId, .Buffer = targets.Counters},
                         RenderGraph::ImportBinding{.Id = argumentsId, .Buffer = targets.Arguments},
                         RenderGraph::ImportBinding{.Id = visitsId, .Buffer = visits}});
        });

    vector<u32> expected(Pixels, 0);
    for (const u32 packed : result.Rays)
    {
        ++expected[Index(Unpack(packed).Pixel)];
    }
    // Each listed pixel appears once, and the dispatch visits exactly those, once each.
    CHECK(std::ranges::all_of(expected, [](const u32 count) { return count <= 1; }));
    CHECK(DownloadAs<u32>(visits) == expected);

    std::filesystem::remove(archive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "ssr classify: the Reflections view shows the chain's reflection")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const path archive = MountPack(assets, "veng_gpu_ssr_classify_debug.vengpack");
    const FloorScene scene = BuildFloor(Context, assets, Types, 0.3f, Framing::Filled);

    SceneRendererSettings settings =
        StochasticSettings(SceneRendererSettings::SsrSamplesPerQuad::One);
    settings.Mode = DebugView::Reflections;
    const Unique<SceneRenderer> renderer = SceneRenderer::Create({
        .Context = Context,
        .Assets = assets,
        .OutputFormat = Context.GetOutputFormat(),
        .Extent = Extent,
        .Settings = settings,
    });
    Context.ImmediateCommands(
        [&](CommandBuffer& cmd)
        {
            renderer->Execute(cmd, Renderer::SceneView{.World = *scene.World,
                                                       .Camera = scene.Camera,
                                                       .Delta = 0.0f,
                                                       .SsrMaxRoughness = MaxRoughness});
        });
    // The chain ran (its lists hold the floor's rays), and the blit shows its denoised reflection:
    // seen from straight above, every ray heads back past the camera and meets nothing.
    CHECK(DownloadAs<u32>(renderer->GetSsrTraceTargets().Counters)[RayCount] == Quads);
    const vector<u8> output = renderer->GetOutput()->GetImage()->Download();
    const auto* halves = reinterpret_cast<const u16*>(output.data());
    f32 brightest = 0.0f;
    for (u32 i = 0; i < Pixels * 4; i += 4)
    {
        brightest =
            std::max({brightest, glm::unpackHalf1x16(halves[i]), glm::unpackHalf1x16(halves[i + 1]),
                      glm::unpackHalf1x16(halves[i + 2])});
    }
    CHECK(brightest == 0.0f);

    std::filesystem::remove(archive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "ssr classify: the lobe sampler draws the GGX distribution of visible normals")
{
    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(path(TEST_SHADER_PACK)).has_value());
    const auto shader = assets.LoadSync<Shader>(SsrLobeSampleCompId);
    REQUIRE_MESSAGE(shader, shader.error().Detail);

    constexpr u32 Side = 64;
    constexpr u32 Samples = Side * Side;
    const Ref<DescriptorSetLayout> setLayout = StorageSetLayout(Context, 1);
    const Ref<ComputePipeline> pipeline =
        MakePipeline(Context, *shader, setLayout, sizeof(LobeSamplePush));
    const Ref<Buffer> directions =
        Buffer::Create(Context, {.Name = "SSR Lobe Directions",
                                 .Size = Samples * sizeof(vec4),
                                 .Usage = BufferUsage::Storage | BufferUsage::TransferSrc});
    const Ref<DescriptorSet> set =
        DescriptorSet::Create(Context, {.Name = "SSR Lobe Sample Set", .Layout = setLayout});
    set->Write(0, directions);

    // A tilted normal, so the sampler's tangent frame is not an axis-aligned special case, and a
    // view 60 degrees from it.
    const vec3 normal = glm::normalize(vec3(0.3f, 0.5f, 0.8f));
    const vec3 tangent = glm::normalize(glm::cross(normal, vec3(1.0f, 0.0f, 0.0f)));
    const f32 viewAngle = glm::radians(60.0f);
    const vec3 toEye = std::cos(viewAngle) * normal + std::sin(viewAngle) * tangent;
    const vec3 mirror = glm::reflect(-toEye, normal);

    for (const f32 alpha : {0.05f, 0.3f})
    {
        CAPTURE(alpha);
        Context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                RenderGraph graph(Context);
                const ResourceId directionsId = graph.ImportBuffer("Directions");
                graph.AddComputePass("Sample Lobe")
                    .StorageBufferWrite(directionsId)
                    .Execute(
                        [&](PassContext& ctx)
                        {
                            ctx.Cmd().BindPipeline(pipeline);
                            ctx.Cmd().BindDescriptorSets(
                                {.Sets = {set},
                                 .FirstSet = 3,
                                 .PipelineBindPoint = PipelineBindPoint::Compute});
                            ctx.Cmd().PushConstants(LobeSamplePush{.ViewRay = vec4(-toEye, 0.0f),
                                                                   .Normal = vec4(normal, 0.0f),
                                                                   .Alpha = alpha,
                                                                   .Side = Side});
                            ctx.Cmd().Dispatch(Samples / 64, 1, 1);
                        });
                auto compiled = graph.Compile();
                compiled->Execute(cmd, std::array{RenderGraph::ImportBinding{
                                           .Id = directionsId, .Buffer = directions}});
            });
        const vector<vec4> drawn = DownloadAs<vec4>(directions);
        REQUIRE(drawn.size() == Samples);

        f64 mirrorCosine = 0.0;
        u32 below = 0;
        f32 unitError = 0.0f;
        f32 leastFacing = 1.0f;
        for (const vec4& sample : drawn)
        {
            const vec3 direction(sample);
            mirrorCosine += glm::dot(direction, mirror);
            below += glm::dot(direction, normal) < 0.0f ? 1 : 0;
            unitError = std::max(unitError, std::abs(glm::length(direction) - 1.0f));
            // The microfacet the view reflected about faces out of the surface: the visible
            // distribution's support. Its normal is the half vector of the view and the result.
            leastFacing = std::min(leastFacing, glm::dot(direction + toEye, normal));
        }
        mirrorCosine /= Samples;
        const f64 belowShare = static_cast<f64>(below) / Samples;

        const LobeReference reference =
            IntegrateVisibleNormals(glm::dvec3(std::sin(static_cast<f64>(viewAngle)), 0.0,
                                               std::cos(static_cast<f64>(viewAngle))),
                                    alpha);
        CAPTURE(mirrorCosine);
        CAPTURE(reference.MeanMirrorCosine);
        CAPTURE(belowShare);
        CAPTURE(reference.BelowSurface);
        CHECK(std::abs(mirrorCosine - reference.MeanMirrorCosine) <=
              0.02 * reference.MeanMirrorCosine);
        // The lobe is not truncated, so a grazing view reflects some of it below the surface, by
        // the share the distribution puts there.
        CHECK(std::abs(belowShare - reference.BelowSurface) <= 0.01);
        CHECK(unitError < 1e-4f);
        CHECK(leastFacing >= -1e-4f);
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "ssr classify: the blue-noise tile is uniform and spreads evenly over windows")
{
    AssetManager assets(Context, Tasks, Types);
    const AssetResult<AssetHandle<RawAsset>> tile = assets.LoadSync<RawAsset>(SsrBlueNoiseId);
    REQUIRE(tile.has_value());
    const vector<u8>& bytes = tile.value().Get()->Bytes;
    constexpr u32 Size = 128;
    REQUIRE(bytes.size() == Size * Size * 2);

    for (u32 channel = 0; channel < 2; ++channel)
    {
        CAPTURE(channel);
        // Each of the 256 levels holds exactly its share of the tile.
        std::array<u32, 256> histogram{};
        for (u32 i = 0; i < Size * Size; ++i)
        {
            ++histogram[bytes[i * 2 + channel]];
        }
        CHECK(std::ranges::all_of(histogram, [](const u32 count) { return count == 64; }));

        // Every aligned 8x8 window averages close to one half; white noise strays about four
        // times further on its worst window.
        f32 worstWindow = 0.0f;
        for (u32 wy = 0; wy < Size; wy += 8)
        {
            for (u32 wx = 0; wx < Size; wx += 8)
            {
                f32 sum = 0.0f;
                for (u32 y = 0; y < 8; ++y)
                {
                    for (u32 x = 0; x < 8; ++x)
                    {
                        sum += (bytes[((wy + y) * Size + wx + x) * 2 + channel] + 0.5f) / 256.0f;
                    }
                }
                worstWindow = std::max(worstWindow, std::abs(sum / 64.0f - 0.5f));
            }
        }
        CAPTURE(worstWindow);
        CHECK(worstWindow < 0.05f);
    }
}
