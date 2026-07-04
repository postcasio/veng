// The Sky material → radiance cubemap bake. Cooks an analytic Sky-domain material whose
// radiance is a continuous, known function of the world view direction (0.5 + 0.5·dir), bakes
// it into the renderer-internal SkyCubemapBake radiance cube, and reads the six faces back to
// assert:
//
//   1. Seam continuity across all twelve cube edges — adjacent faces must agree exactly along
//      every shared edge, or the per-face view-ray basis disagrees and the cube shows seams.
//   2. The analytic per-direction radiance — each baked texel equals the material's function of
//      the direction its face basis reconstructs, so the bake supplies the right basis and the
//      material fragment is genuinely reused.
//   3. In-place re-bake — a second bake of the same instance reuses the one cube (no realloc).
//   4. The baked cube round-trips the skybox display path — a baked material sky rendered through
//      the SceneRenderer matches the same material rendered direct (the two modes agree), and the
//      renderer flips direct↔baked with an internal recompile (output identity preserved).
//
// The final cases cover the procedural atmosphere as a cubemap producer: a baked atmosphere
// rendered through the SceneRenderer matches the same atmosphere rendered direct (the shared
// bake basis + LUT sample), and its baked cube feeds both the SH and IBL lighting tiers — the
// tiers a direct atmosphere could not reach.
//
// Gated on GPU_GBUFFER_FIXTURE_DIR (the in-process cook of the analytic Sky material; the
// atmosphere cases are procedural and need no cook, but share the file's fixture wiring).

#include <array>
#include <cmath>

#include <doctest/doctest.h>

#include <gpu/fixture.h>

#ifdef GPU_GBUFFER_FIXTURE_DIR

#include <filesystem>

#include <glm/gtc/packing.hpp>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Math/SphericalHarmonics.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSetLayout.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include "Renderer/EnvironmentIbl.h"
#include "Renderer/SkyCubemapBake.h"

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr AssetId AnalyticSkyInstanceId{0x00000000000024A1ULL};

    // The world direction a face texel reconstructs, matching ibl_equirect_to_cube's FaceDirection
    // and the InvViewProj basis SkyCubemapBake builds — the seam/radiance oracle's ground truth.
    vec3 FaceDirection(u32 face, vec2 uv)
    {
        const vec2 st = uv * 2.0f - 1.0f;
        vec3 dir(0.0f);
        switch (face)
        {
        case 0:
            dir = vec3(1.0f, -st.y, -st.x);
            break; // +X
        case 1:
            dir = vec3(-1.0f, -st.y, st.x);
            break; // -X
        case 2:
            dir = vec3(st.x, 1.0f, st.y);
            break; // +Y
        case 3:
            dir = vec3(st.x, -1.0f, -st.y);
            break; // -Y
        case 4:
            dir = vec3(st.x, -st.y, 1.0f);
            break; // +Z
        default:
            dir = vec3(-st.x, -st.y, -1.0f);
            break; // -Z
        }
        return glm::normalize(dir);
    }

    // One RGBA16F cube texel (layer-major, one face after another) decoded to a linear vec3.
    vec3 DecodeTexel(const vector<u8>& rgba16f, u32 faceSize, u32 face, u32 x, u32 y)
    {
        const auto* halves = reinterpret_cast<const u16*>(rgba16f.data());
        const usize base = ((static_cast<usize>(face) * faceSize + y) * faceSize + x) * 4;
        return vec3(glm::unpackHalf1x16(halves[base + 0]), glm::unpackHalf1x16(halves[base + 1]),
                    glm::unpackHalf1x16(halves[base + 2]));
    }

    // Downloads all six cube layers (Image::Download reads only layer 0). Transitions the cube to
    // TransferSrc, copies the six layers into one tightly-packed staging buffer (layer-major), and
    // restores the sampled layout — the seam/radiance oracle reads every face.
    vector<u8> DownloadCube(Context& context, SkyCubemapBake& bake)
    {
        const u32 faceSize = bake.GetFaceSize();
        const usize faceBytes = static_cast<usize>(faceSize) * faceSize * 8; // RGBA16F
        const Ref<Buffer> staging = Buffer::Create(context, {
                                                                .Name = "Sky Bake Readback",
                                                                .Size = faceBytes * 6,
                                                                .Usage = BufferUsage::TransferDst,
                                                            });

        context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                cmd.PrepareForAccess(bake.GetCubeView(), AccessKind::TransferSrc);
                const vk::BufferImageCopy region{
                    .bufferOffset = 0,
                    .bufferRowLength = 0,
                    .bufferImageHeight = 0,
                    .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                         .mipLevel = 0,
                                         .baseArrayLayer = 0,
                                         .layerCount = 6},
                    .imageOffset = {.x = 0, .y = 0, .z = 0},
                    .imageExtent = {.width = faceSize, .height = faceSize, .depth = 1},
                };
                Renderer::GetVkCommandBuffer(cmd).copyImageToBuffer(
                    Renderer::GetVkImage(*bake.GetCubeImage()),
                    vk::ImageLayout::eTransferSrcOptimal, Renderer::GetVkBuffer(*staging), 1,
                    &region);
                cmd.PrepareForAccess(bake.GetCubeView(), AccessKind::Sample);
            });

        return staging->Download();
    }

    AssetHandle<MaterialInstance> CookAndLoadAnalyticSky(AssetManager& assets)
    {
        const path fixtureDir = path(GPU_GBUFFER_FIXTURE_DIR);
        const path outArchive =
            std::filesystem::temp_directory_path() / "veng_gpu_sky_bake.vengpack";

        Cook::Cooker cooker;
        Cook::RegisterBuiltinImporters(cooker);
        const VoidResult cookResult =
            cooker.CookPack(fixtureDir / "gbuffer_pack.json", outArchive, {}, nullptr, nullptr,
                            nullptr, nullptr, {}, path(VENG_CORE_SHADER_DIR));
        REQUIRE(cookResult.has_value());

        REQUIRE(assets.Mount(outArchive).has_value());
        const AssetResult<AssetHandle<MaterialInstance>> material =
            assets.LoadSync<MaterialInstance>(AnalyticSkyInstanceId);
        REQUIRE(material.has_value());
        REQUIRE(material->IsLoaded());
        return *material;
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "sky bake: an analytic Sky material bakes to a seamless radiance cube matching "
                  "its per-direction radiance, re-baking in place")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const AssetHandle<MaterialInstance> material = CookAndLoadAnalyticSky(assets);

    // The bake wants an IBL consumer set layout (the skybox path's radiance-set shape); the
    // renderer owns one, but the bake only needs the layout, so build a standalone IBL for it.
    const Unique<EnvironmentIbl> ibl = EnvironmentIbl::Create(Context, assets);

    constexpr u32 FaceSize = 64;
    const Unique<SkyCubemapBake> bake =
        SkyCubemapBake::Create(Context, ibl->GetSetLayout(), Format::RGBA16Sfloat, FaceSize);

    // The bake reads the material's ring-buffered param block, written into the current frame's
    // region eagerly on register, so it is resident without a frame acquire. Bake claims one view
    // slot per face (six of MaxViewsPerFrame), well within the ring.
    const Ref<Image> cubeImage = bake->GetCubeImage();
    Context.ImmediateCommands([&](CommandBuffer& cmd) { bake->Bake(cmd, *material.Get()); });

    // Re-baking the same instance reuses the one cube image (no realloc): VRAM is one cube.
    Context.ImmediateCommands([&](CommandBuffer& cmd) { bake->Bake(cmd, *material.Get()); });
    CHECK(bake->GetCubeImage().get() == cubeImage.get());

    const vector<u8> faces = DownloadCube(Context, *bake);
    REQUIRE(faces.size() == static_cast<usize>(FaceSize) * FaceSize * 6 * 8);

    // The material writes 0.5 + 0.5·dir. Each baked texel must equal that function of the
    // direction its face basis reconstructs — the analytic-radiance oracle. A generous epsilon
    // covers the RGBA16F round-trip and the fragment's fp reconstruction.
    constexpr f32 Eps = 0.02f;
    for (u32 face = 0; face < 6; ++face)
    {
        for (u32 y = 0; y < FaceSize; ++y)
        {
            for (u32 x = 0; x < FaceSize; ++x)
            {
                const vec2 uv((static_cast<f32>(x) + 0.5f) / static_cast<f32>(FaceSize),
                              (static_cast<f32>(y) + 0.5f) / static_cast<f32>(FaceSize));
                const vec3 expected = 0.5f + 0.5f * FaceDirection(face, uv);
                const vec3 actual = DecodeTexel(faces, FaceSize, face, x, y);
                REQUIRE(glm::length(actual - expected) < Eps);
            }
        }
    }

    // Explicit twelve-edge seam continuity: for each shared edge of two faces, the two faces'
    // border texels along it map to (nearly) the same world direction, so they must carry the
    // same radiance. Compare each border texel of face A against the texel of face B whose
    // reconstructed direction is closest — a seam shows up as a border mismatch. Because both
    // faces evaluate the same analytic function of direction, agreement proves the basis is
    // consistent across the edge.
    auto BorderTexels = [&](u32 face) -> vector<std::pair<vec3, vec3>>
    {
        // Returns (direction, color) for every border texel of a face.
        vector<std::pair<vec3, vec3>> out;
        for (u32 i = 0; i < FaceSize; ++i)
        {
            const std::array<uvec2, 4> border = {uvec2(i, 0), uvec2(i, FaceSize - 1), uvec2(0, i),
                                                 uvec2(FaceSize - 1, i)};
            for (const uvec2 t : border)
            {
                const vec2 uv((static_cast<f32>(t.x) + 0.5f) / static_cast<f32>(FaceSize),
                              (static_cast<f32>(t.y) + 0.5f) / static_cast<f32>(FaceSize));
                out.emplace_back(FaceDirection(face, uv),
                                 DecodeTexel(faces, FaceSize, face, t.x, t.y));
            }
        }
        return out;
    };

    // The six adjacency pairs cover all twelve edges (each pair shares one edge; the +X/-X and
    // +Z/-Z-type opposite pairs do not touch, so the adjacency list is the twelve face-pairs
    // that share an edge). Walk every ordered adjacent pair and, for each border texel of A near
    // the shared edge, find B's nearest-direction border texel and require the colors agree.
    constexpr std::array<std::pair<u32, u32>, 12> edges = {{
        {0, 2},
        {0, 3},
        {0, 4},
        {0, 5}, // +X to +Y,-Y,+Z,-Z
        {1, 2},
        {1, 3},
        {1, 4},
        {1, 5}, // -X to +Y,-Y,+Z,-Z
        {2, 4},
        {2, 5}, // +Y to +Z,-Z
        {3, 4},
        {3, 5}, // -Y to +Z,-Z
    }};

    for (const auto& [fa, fb] : edges)
    {
        const vector<std::pair<vec3, vec3>> a = BorderTexels(fa);
        const vector<std::pair<vec3, vec3>> b = BorderTexels(fb);
        // Find the closest-direction border-texel pairs across the two faces; the ones on the
        // shared edge are near-coincident in direction. Only those (angular gap below a texel's
        // worth) are required to agree in color — a seam is a color jump between them.
        const f32 texelAngle = 2.0f / static_cast<f32>(FaceSize); // ~edge of a texel in st units
        for (const auto& [dirA, colA] : a)
        {
            f32 best = 1e9f;
            vec3 bestCol(0.0f);
            for (const auto& [dirB, colB] : b)
            {
                const f32 d = glm::length(dirA - dirB);
                if (d < best)
                {
                    best = d;
                    bestCol = colB;
                }
            }
            if (best < texelAngle)
            {
                CHECK(glm::length(colA - bestCol) < Eps);
            }
        }
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "sky bake: the baked cube feeds the cube→SH ambient projection — display and "
                  "ambient read the one cube")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const AssetHandle<MaterialInstance> material = CookAndLoadAnalyticSky(assets);

    const Unique<EnvironmentIbl> ibl = EnvironmentIbl::Create(Context, assets);

    constexpr u32 FaceSize = 64;
    const Unique<SkyCubemapBake> bake =
        SkyCubemapBake::Create(Context, ibl->GetSetLayout(), Format::RGBA16Sfloat, FaceSize);

    // Bake into the one cube, then read it back and project it to the irradiance SH the cheap
    // ambient arm evaluates — the same cube the skybox display path samples through GetSet().
    Context.ImmediateCommands([&](CommandBuffer& cmd) { bake->Bake(cmd, *material.Get()); });
    const vector<u8> faces = DownloadCube(Context, *bake);
    const Sh9 sh = EnvironmentIbl::ProjectCubeToIrradianceSh(faces, bake->GetFaceSize());

    // The material writes 0.5 + 0.5·dir, so the sky is brightest toward +dir on each axis. The
    // convolved diffuse irradiance must therefore be greater facing +X than -X (the l=1 term the
    // directional radiance carries) and strictly positive — a flat or zeroed projection would fail.
    const vec3 plusX = EvalIrradiance(sh, vec3(1, 0, 0));
    const vec3 minusX = EvalIrradiance(sh, vec3(-1, 0, 0));
    CHECK(plusX.r > minusX.r);
    CHECK(plusX.r > 0.0f);

    // BakeAndDownload is the renderer's self-contained readback: it bakes the same cube and returns
    // the same radiance the display bake produced, so its projection matches within the round-trip.
    const vector<u8> selfContained = bake->BakeAndDownload(*material.Get());
    const Sh9 shSelf =
        EnvironmentIbl::ProjectCubeToIrradianceSh(selfContained, bake->GetFaceSize());
    const vec3 plusXSelf = EvalIrradiance(shSelf, vec3(1, 0, 0));
    CHECK(plusXSelf.r == doctest::Approx(plusX.r).epsilon(0.05));
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "sky bake: a baked material sky renders through the skybox path and matches the "
                  "same material rendered direct, flipping mode with an internal recompile")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const AssetHandle<MaterialInstance> material = CookAndLoadAnalyticSky(assets);

    constexpr uvec2 extent{96, 96};

    // An empty scene apart from the material sky: every pixel is background the sky fills.
    const Unique<Scene> scene = Scene::Create(Types);
    const Entity skyEntity = scene->CreateEntity();
    Sky& sky = scene->Add<Sky>(skyEntity);
    auto* source = static_cast<MaterialSky*>(sky.Source.SetActive(TypeIdOf<MaterialSky>()));
    source->Material = material;

    CameraView camera;
    camera.SetPerspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
    camera.SetView(vec3(0.0f), vec3(0.3f, 0.2f, -1.0f), vec3(0.0f, 1.0f, 0.0f));

    const Unique<SceneRenderer> renderer = SceneRenderer::Create({
        .Context = Context,
        .Assets = assets,
        .OutputFormat = Context.GetOutputFormat(),
        .Extent = extent,
        .Settings = {.Mode = DebugView::Final, .Bloom = false, .Shadows = false, .AO = false},
    });

    auto RenderWithMode = [&](SkyMode mode) -> vector<u8>
    {
        source->Mode = mode;
        Context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                renderer->Execute(
                    cmd, Renderer::SceneView{.World = *scene, .Camera = camera, .Delta = 0.0f});
            });
        const vector<u8> pixels = renderer->GetOutput()->GetImage()->Download();
        REQUIRE(pixels.size() == static_cast<usize>(extent.x) * extent.y * 8);
        return pixels;
    };

    const vector<u8> direct = RenderWithMode(SkyMode::Direct);
    // The mode flip is an internal recompile preserving output identity; the baked cube samples
    // the same radiance the direct pass composites, so the two frames agree bar the cube's
    // bilinear filtering + face resolution. Compare a spread of background texels.
    const vector<u8> baked = RenderWithMode(SkyMode::Baked);

    const auto* dHalves = reinterpret_cast<const u16*>(direct.data());
    const auto* bHalves = reinterpret_cast<const u16*>(baked.data());

    f64 directLuma = 0.0;
    f64 diff = 0.0;
    for (usize i = 0; i < static_cast<usize>(extent.x) * extent.y; ++i)
    {
        const vec3 d(glm::unpackHalf1x16(dHalves[i * 4 + 0]),
                     glm::unpackHalf1x16(dHalves[i * 4 + 1]),
                     glm::unpackHalf1x16(dHalves[i * 4 + 2]));
        const vec3 b(glm::unpackHalf1x16(bHalves[i * 4 + 0]),
                     glm::unpackHalf1x16(bHalves[i * 4 + 1]),
                     glm::unpackHalf1x16(bHalves[i * 4 + 2]));
        directLuma += 0.2126 * d.r + 0.7152 * d.g + 0.0722 * d.b;
        diff += glm::length(d - b);
    }
    const f64 pixelCount = static_cast<f64>(extent.x) * extent.y;
    // Both modes composited a non-black sky.
    CHECK(directLuma / pixelCount > 0.0);
    // The two modes agree to within the cube-resolution filtering tolerance (a low mean per-pixel
    // difference); a broken basis or a mis-sampled cube would diverge sharply.
    CHECK(diff / pixelCount < 0.06);
}

namespace
{
    // Builds a full-sky atmosphere scene (an AtmosphereSky plus a directional sun, whose inverse
    // travel direction is the toward-sun direction the sky and any lighting share) and a renderer.
    // The caller drives Mode/tier through the returned Render lambda; each Execute claims bake +
    // frame view slots, so a case keeps its Execute count within one frame's view budget (16).
    struct AtmosphereSceneFixture
    {
        Unique<Scene> World;
        Sky* SkyComponent = nullptr;
        AtmosphereSky* Source = nullptr;
        CameraView Camera;
        Unique<SceneRenderer> Renderer;
        Context& Ctx;
        uvec2 Extent;

        AtmosphereSceneFixture(Context& context, AssetManager& assets, TypeRegistry& types,
                               uvec2 extent)
            : World(Scene::Create(types)), Ctx(context), Extent(extent)
        {
            const Entity skyEntity = World->CreateEntity();
            SkyComponent = &World->Add<Sky>(skyEntity);
            auto* source = static_cast<AtmosphereSky*>(
                SkyComponent->Source.SetActive(TypeIdOf<AtmosphereSky>()));
            Source = source;
            SkyComponent->Intensity = 1.0f;

            const Entity sunEntity = World->CreateEntity();
            auto& sun = World->Add<Light>(sunEntity);
            sun.Type = LightType::Directional;
            sun.Direction = glm::normalize(vec3(0.3f, -0.4f, -0.6f)); // a low sun

            Camera.SetPerspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
            Camera.SetView(vec3(0.0f), vec3(0.2f, 0.1f, -1.0f), vec3(0.0f, 1.0f, 0.0f));

            Renderer = SceneRenderer::Create({
                .Context = context,
                .Assets = assets,
                .OutputFormat = context.GetOutputFormat(),
                .Extent = extent,
                .Settings =
                    {.Mode = DebugView::Final, .Bloom = false, .Shadows = false, .AO = false},
            });
        }

        vector<u8> Render(SkyMode mode, SkyLighting tier)
        {
            Source->Mode = mode;
            SkyComponent->Lighting = tier;
            Ctx.ImmediateCommands(
                [&](CommandBuffer& cmd)
                {
                    Renderer->Execute(
                        cmd, Renderer::SceneView{.World = *World, .Camera = Camera, .Delta = 0.0f});
                });
            vector<u8> pixels = Renderer->GetOutput()->GetImage()->Download();
            REQUIRE(pixels.size() == static_cast<usize>(Extent.x) * Extent.y * 8);
            return pixels;
        }
    };

    f64 MeanLuma(const vector<u8>& px, uvec2 extent)
    {
        const auto* h = reinterpret_cast<const u16*>(px.data());
        f64 luma = 0.0;
        for (usize i = 0; i < static_cast<usize>(extent.x) * extent.y; ++i)
        {
            const vec3 c(glm::unpackHalf1x16(h[i * 4 + 0]), glm::unpackHalf1x16(h[i * 4 + 1]),
                         glm::unpackHalf1x16(h[i * 4 + 2]));
            luma += 0.2126 * c.r + 0.7152 * c.g + 0.0722 * c.b;
        }
        return luma / (static_cast<f64>(extent.x) * extent.y);
    }
}

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "atmosphere bake: a baked atmosphere renders through the skybox path, matches the "
    "same atmosphere rendered direct, and lights via IBL")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);

    constexpr uvec2 extent{96, 96};
    AtmosphereSceneFixture fixture(Context, assets, Types, extent);

    // Baked-vs-direct agreement: the baked cube samples the same LUT-derived radiance the direct
    // pass composites per pixel, so the two frames agree bar the cube's bilinear filtering + face
    // resolution. A broken face basis or a mis-sampled cube would diverge sharply.
    const vector<u8> direct = fixture.Render(SkyMode::Direct, SkyLighting::None);
    const vector<u8> baked = fixture.Render(SkyMode::Baked, SkyLighting::None);

    const auto* dHalves = reinterpret_cast<const u16*>(direct.data());
    const auto* bHalves = reinterpret_cast<const u16*>(baked.data());
    f64 diff = 0.0;
    for (usize i = 0; i < static_cast<usize>(extent.x) * extent.y; ++i)
    {
        const vec3 d(glm::unpackHalf1x16(dHalves[i * 4 + 0]),
                     glm::unpackHalf1x16(dHalves[i * 4 + 1]),
                     glm::unpackHalf1x16(dHalves[i * 4 + 2]));
        const vec3 b(glm::unpackHalf1x16(bHalves[i * 4 + 0]),
                     glm::unpackHalf1x16(bHalves[i * 4 + 1]),
                     glm::unpackHalf1x16(bHalves[i * 4 + 2]));
        diff += glm::length(d - b);
    }
    const f64 pixelCount = static_cast<f64>(extent.x) * extent.y;
    CHECK(MeanLuma(direct, extent) > 0.0); // a non-black sky rendered
    CHECK(diff / pixelCount < 0.08); // the two modes agree within the cube-resolution tolerance

    // IBL, the cell that was impossible for an atmosphere before it became a cube producer, now
    // runs (its baked cube convolves into the split-sum maps) and renders a plausible sky.
    const vector<u8> bakedIbl = fixture.Render(SkyMode::Baked, SkyLighting::IBL);
    CHECK(MeanLuma(bakedIbl, extent) > 0.0);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "atmosphere bake: a baked atmosphere lights via SH from its baked cube")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);

    constexpr uvec2 extent{96, 96};
    AtmosphereSceneFixture fixture(Context, assets, Types, extent);

    // The SH tier, previously rejected-with-log on an atmosphere source, now projects the baked
    // cube to the skylight SH (the same cube→SH path every source shares). A full-sky frame renders
    // validation-clean and non-black — the tier path produces a plausible sky, not a zeroed frame.
    const vector<u8> bakedSh = fixture.Render(SkyMode::Baked, SkyLighting::SH);
    CHECK(MeanLuma(bakedSh, extent) > 0.0);
}

#endif
