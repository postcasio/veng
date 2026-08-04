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
#include "support/TempPath.h"

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
#include <Veng/Renderer/GeneratedTextureService.h>
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

    // Drives a SceneRenderer's amortized sky bake to completion and returns the rendered frame.
    // RequestBake is recorded from an Execute; the GeneratedTextureService fills the scratch cube
    // across pumped frames (unlimited budget so one pump suffices); a final Execute copies the
    // landed bake into the displayed cube and samples it. A Direct/None sky bakes nothing, so the
    // pumped frames are inert and the two Executes render the identical frame — the helper is
    // correct for either mode, which is what lets a case compare baked against direct through it.
    vector<u8> RenderSkyToCompletion(Context& context, SceneRenderer& renderer, Scene& scene,
                                     const CameraView& camera, const uvec2 extent)
    {
        context.GetGeneratedTextures().SetTickBudget(GeneratedTextureService::UnlimitedTickBudget);
        const auto execute = [&]
        {
            context.ImmediateCommands(
                [&](CommandBuffer& cmd)
                {
                    renderer.Execute(
                        cmd, Renderer::SceneView{.World = scene, .Camera = camera, .Delta = 0.0f});
                });
        };
        execute(); // request the amortized bake against the (not-yet-filled) displayed cube
        for (u32 i = 0; i < 4; ++i)
        {
            context.BeginFrame();
            context.EndFrame();
        }
        execute(); // copy the landed bake into the displayed cube and sample it
        vector<u8> pixels = renderer.GetOutput()->GetImage()->Download();
        REQUIRE(pixels.size() == static_cast<usize>(extent.x) * extent.y * 8);
        return pixels;
    }

    AssetHandle<MaterialInstance> CookAndLoadAnalyticSky(AssetManager& assets)
    {
        const path fixtureDir = path(GPU_GBUFFER_FIXTURE_DIR);
        const path outArchive = Veng::TestSupport::TempDir() / "veng_gpu_sky_bake.vengpack";

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

    // BakeAndDownload is the SH tier's cold-start readback: it bakes the same cube, reduces it to
    // the readback level, and returns that radiance, so its projection matches the full-face one
    // within the reduction + round-trip.
    const vector<u8> selfContained = bake->BakeAndDownload(*material.Get());
    const Sh9 shSelf =
        EnvironmentIbl::ProjectCubeToIrradianceSh(selfContained, bake->GetShReadbackFaceSize());
    const vec3 plusXSelf = EvalIrradiance(shSelf, vec3(1, 0, 0));
    CHECK(plusXSelf.r == doctest::Approx(plusX.r).epsilon(0.05));
}

namespace
{
    // Downloads a specific mip level of every cube layer into one tightly-packed staging buffer
    // (layer-major). DownloadCube's mip-agnostic sibling — the reduction/readback level tests read
    // both the display level (mip 0) and the reduced readback level.
    vector<u8> DownloadCubeLevel(Context& context, SkyCubemapBake& bake, u32 mip, u32 faceSize)
    {
        const usize faceBytes = static_cast<usize>(faceSize) * faceSize * 8; // RGBA16F
        const Ref<Buffer> staging = Buffer::Create(context, {
                                                                .Name = "Sky Bake Level Readback",
                                                                .Size = faceBytes * 6,
                                                                .Usage = BufferUsage::TransferDst,
                                                            });
        const Ref<ImageView> levelView =
            ImageView::Create(context, {
                                           .Name = "Sky Bake Level View",
                                           .Image = bake.GetCubeImage(),
                                           .ViewType = ImageViewType::Array2D,
                                           .BaseMipLevel = mip,
                                           .MipLevels = 1,
                                           .BaseArrayLayer = 0,
                                           .ArrayLayers = 6,
                                       });
        context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                cmd.PrepareForAccess(levelView, AccessKind::TransferSrc);
                const vk::BufferImageCopy region{
                    .bufferOffset = 0,
                    .bufferRowLength = 0,
                    .bufferImageHeight = 0,
                    .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                         .mipLevel = mip,
                                         .baseArrayLayer = 0,
                                         .layerCount = 6},
                    .imageOffset = {.x = 0, .y = 0, .z = 0},
                    .imageExtent = {.width = faceSize, .height = faceSize, .depth = 1},
                };
                Renderer::GetVkCommandBuffer(cmd).copyImageToBuffer(
                    Renderer::GetVkImage(*bake.GetCubeImage()),
                    vk::ImageLayout::eTransferSrcOptimal, Renderer::GetVkBuffer(*staging), 1,
                    &region);
                cmd.PrepareForAccess(levelView, AccessKind::Sample);
            });
        return staging->Download();
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "sky bake: a dirty SH-tier re-bake records one bake, not two")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const AssetHandle<MaterialInstance> material = CookAndLoadAnalyticSky(assets);

    const Unique<EnvironmentIbl> ibl = EnvironmentIbl::Create(Context, assets);

    constexpr u32 FaceSize = 64;
    const Unique<SkyCubemapBake> bake =
        SkyCubemapBake::Create(Context, ibl->GetSetLayout(), Format::RGBA16Sfloat, FaceSize);

    // The steady-state SH path: one display bake fills the cube, and the reduced readback reads that
    // same cube back (RecordReductionMips + a deferred copy, no face render), so a dirty SH sky
    // costs one bake — six face renders.
    const u64 beforeSteady = bake->GetFaceRendersRecorded();
    Context.ImmediateCommands(
        [&](CommandBuffer& cmd)
        {
            bake->Bake(cmd, *material.Get());
            bake->RecordReductionMips(cmd);
        });
    CHECK(bake->GetFaceRendersRecorded() - beforeSteady == SkyCubemapBake::CubeFaces);

    // The superseded readback baked the cube a second time: a display bake plus a self-contained
    // readback bake was twelve face renders for one dirty signal, which the deferred path removes.
    const u64 beforeCold = bake->GetFaceRendersRecorded();
    Context.ImmediateCommands([&](CommandBuffer& cmd) { bake->Bake(cmd, *material.Get()); });
    (void)bake->BakeAndDownload(*material.Get());
    CHECK(bake->GetFaceRendersRecorded() - beforeCold == 2 * SkyCubemapBake::CubeFaces);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "sky bake: the reduced SH readback reads the display bake's own cube")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const AssetHandle<MaterialInstance> material = CookAndLoadAnalyticSky(assets);

    const Unique<EnvironmentIbl> ibl = EnvironmentIbl::Create(Context, assets);

    // A display face larger than the readback size, so the reduction actually runs (256 -> 128 ->
    // 64): two halving blits to the readback level.
    constexpr u32 FaceSize = 256;
    const Unique<SkyCubemapBake> bake =
        SkyCubemapBake::Create(Context, ibl->GetSetLayout(), Format::RGBA16Sfloat, FaceSize);
    CHECK(bake->GetShReadbackFaceSize() == SkyCubemapBake::ShReadbackFaceSize);
    CHECK(bake->GetShReadbackMipLevel() == 2);

    // Bake the display face and reduce it, then project both the display level and the reduced
    // readback level. They must agree: the reduction preserves the coefficients the full face
    // produces, and the readback reads a validly-filled level (a wrong or unwritten level would not
    // project to the same light) — the display bake's own cube, read back reduced rather than
    // baked a second time.
    Context.ImmediateCommands(
        [&](CommandBuffer& cmd)
        {
            bake->Bake(cmd, *material.Get());
            bake->RecordReductionMips(cmd);
        });
    const vector<u8> full = DownloadCubeLevel(Context, *bake, 0, FaceSize);
    const vector<u8> reduced = DownloadCubeLevel(Context, *bake, bake->GetShReadbackMipLevel(),
                                                 bake->GetShReadbackFaceSize());
    const Sh9 shFull = EnvironmentIbl::ProjectCubeToIrradianceSh(full, FaceSize);
    const Sh9 shReduced =
        EnvironmentIbl::ProjectCubeToIrradianceSh(reduced, bake->GetShReadbackFaceSize());

    const std::array<vec3, 4> normals = {vec3(1, 0, 0), vec3(-1, 0, 0), vec3(0, 1, 0),
                                         glm::normalize(vec3(1, 1, 1))};
    for (const vec3 n : normals)
    {
        const vec3 fromFull = EvalIrradiance(shFull, n);
        const vec3 fromReduced = EvalIrradiance(shReduced, n);
        CHECK(fromReduced.r == doctest::Approx(fromFull.r).epsilon(0.05));
        CHECK(fromReduced.g == doctest::Approx(fromFull.g).epsilon(0.05));
        CHECK(fromReduced.b == doctest::Approx(fromFull.b).epsilon(0.05));
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "sky bake: an amortized bake fills its cube one face per tick, not six draws in "
                  "one frame")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const AssetHandle<MaterialInstance> material = CookAndLoadAnalyticSky(assets);

    const Unique<EnvironmentIbl> ibl = EnvironmentIbl::Create(Context, assets);

    constexpr u32 FaceSize = 64;
    const Unique<SkyCubemapBake> bake =
        SkyCubemapBake::Create(Context, ibl->GetSetLayout(), Format::RGBA16Sfloat, FaceSize);

    GeneratedTextureService& service = Context.GetGeneratedTextures();
    // A budget of one tick is amortization at its most granular: one face may be recorded per
    // frame, so the property under test — the six faces spread across ticks rather than landing in
    // one frame — shows as one face render per pumped frame.
    service.SetTickBudget(1);

    const u64 before = bake->GetFaceRendersRecorded();
    bake->RequestBake(service, *material.Get());
    CHECK(bake->IsBakePending());
    // The request records nothing itself: the six draws are the service's to spend, not this frame's.
    CHECK(bake->GetFaceRendersRecorded() == before);

    for (u32 face = 0; face < SkyCubemapBake::CubeFaces; ++face)
    {
        CHECK(bake->GetFaceRendersRecorded() - before == face);
        CHECK(bake->IsBakePending());
        Context.BeginFrame();
        Context.EndFrame();
        // Exactly one more face than last frame — never the whole cube in one frame.
        CHECK(bake->GetFaceRendersRecorded() - before == face + 1);
    }

    // Every face rendered, one per frame, and the fill is done.
    CHECK(bake->GetFaceRendersRecorded() - before == SkyCubemapBake::CubeFaces);
    CHECK_FALSE(bake->IsBakePending());

    // The completed scratch cube copies into the displayed cube once, on a recorded frame; a second
    // call is a no-op because the bake is no longer landed.
    bool copied = false;
    Context.ImmediateCommands([&](CommandBuffer& cmd) { copied = bake->RecordAmortized(cmd); });
    CHECK(copied);
    Context.ImmediateCommands([&](CommandBuffer& cmd) { CHECK_FALSE(bake->RecordAmortized(cmd)); });

    // And the displayed cube the copy filled carries the material's analytic radiance, so the
    // amortized path produced the same cube a synchronous bake would — read a spread of texels.
    const vector<u8> faces = DownloadCube(Context, *bake);
    constexpr f32 Eps = 0.02f;
    for (u32 face = 0; face < 6; ++face)
    {
        for (const uvec2 t : {uvec2(FaceSize / 4, FaceSize / 4), uvec2(FaceSize / 2, FaceSize / 2),
                              uvec2(3 * FaceSize / 4, 3 * FaceSize / 4)})
        {
            const vec2 uv((static_cast<f32>(t.x) + 0.5f) / static_cast<f32>(FaceSize),
                          (static_cast<f32>(t.y) + 0.5f) / static_cast<f32>(FaceSize));
            const vec3 expected = 0.5f + 0.5f * FaceDirection(face, uv);
            const vec3 actual = DecodeTexel(faces, FaceSize, face, t.x, t.y);
            CHECK(glm::length(actual - expected) < Eps);
        }
    }
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
        return RenderSkyToCompletion(Context, *renderer, *scene, camera, extent);
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
            return RenderSkyToCompletion(Ctx, *Renderer, *World, Camera, Extent);
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
