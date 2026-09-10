// SceneCapture's opt-in radiance cube (SceneCaptureInfo::Cube), driven end to end through the real
// six-face capture and fed to the real IBL convolution. A scene places a bright green self-lit cube
// directly below the probe (world -Y), out of the probe's other faces; after a full six-face sweep
// the capture publishes a cube view, which EnvironmentIbl::GenerateFromCube convolves. The cases
// assert:
//
//  - the cube is a valid IBL source that carries the scene, oriented correctly: the convolved
//    irradiance is greatest and green-dominant on the -Y face (where the green cube sits) and near
//    black on the +Y face — a directional assert, since a wrong layer/axis mapping would land the
//    colour on the wrong face and a colour-only check could not catch it;
//  - the revision advances only on a completed sweep: fewer than six pushed faces leave it at zero,
//    the sixth advances it, and a second six advances it again — the completion signal a consumer
//    derives on;
//  - the Environment layer is kept by a capture at the shared default and dropped only when the
//    capture clears it: the green cube on RenderLayer::Environment reaches the cube under
//    DefaultEnvironmentCaptureLayers, and is absent when the capture names Default alone.
//
// Skips cleanly (exit 77) on a machine with no Vulkan ICD, like the rest of the gpu band.

#include <cmath>
#include <vector>

#include <doctest/doctest.h>

#include <glm/gtc/packing.hpp>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Primitives.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/SceneCapture.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/RenderLayer.h>
#include <Veng/Scene/Scene.h>

#include <gpu/fixture.h>
#include "support/TempPath.h"

#include "Renderer/EnvironmentIbl.h"

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr u32 CubeFaces = 6;
    constexpr u32 CaptureFaceSize = 64;

    // The green self-lit backdrop instance in the shared capture fixture pack (Color = (0, 4, 0)).
    constexpr AssetId BackdropInstance{0x2452};

    // Cook the shared capture fixture pack in-process; the core pack (auto-mounted) supplies the
    // cube-face copy shader the capture builds its cube pipeline from.
    path CookPack()
    {
        const path fixtureDir = path(GPU_GBUFFER_FIXTURE_DIR);
        const path outArchive = Veng::TestSupport::TempDir() / "veng_scene_capture_cube.vengpack";
        Cook::Cooker cooker;
        Cook::RegisterBuiltinImporters(cooker);
        const VoidResult cooked =
            cooker.CookPack(fixtureDir / "capture_surface_pack.json", outArchive, {}, nullptr,
                            nullptr, nullptr, nullptr, {}, path(VENG_CORE_SHADER_DIR));
        REQUIRE(cooked.has_value());
        return outArchive;
    }

    // A scene: one green self-lit cube directly below the origin (world -Y), on the given layer.
    Unique<Scene> BuildScene(Context& context, AssetManager& assets, TypeRegistry& types,
                             const AssetHandle<MaterialInstance>& material, RenderLayer layer,
                             vector<Ref<Mesh>>& meshes)
    {
        Unique<Scene> scene = Scene::Create(types);
        const Ref<Mesh> cube =
            Mesh::BuildSync(context, Primitives::Cube(6.0f, material), "Backdrop");
        meshes.push_back(cube);
        const Entity entity = scene->CreateEntity();
        scene->Add<Transform>(entity).Position = vec3(0.0f, -8.0f, 0.0f);
        auto& mesh = scene->Add<MeshRenderer>(entity);
        mesh.Mesh = assets.Adopt(cube);
        mesh.Layer = layer;
        return scene;
    }

    SceneRendererSettings LeanSettings()
    {
        SceneRendererSettings settings;
        settings.Bloom = false;
        settings.Shadows = false;
        settings.PunctualShadows = false;
        settings.AO = false;
        settings.SSR = false;
        settings.PostProcessEffects = false;
        settings.AntiAliasing = AntiAliasingMode::None;
        return settings;
    }

    // Downloads all six irradiance-cube layers into one tightly-packed RGBA16F buffer (layer-major).
    std::vector<u8> DownloadIrradiance(Context& context, EnvironmentIbl& ibl)
    {
        const u32 faceSize = EnvironmentIbl::GetIrradianceFaceSize();
        const usize faceBytes = static_cast<usize>(faceSize) * faceSize * 8;
        const Ref<Image>& image = ibl.GetIrradianceImage();
        const Ref<ImageView> view = ImageView::Create(context, {
                                                                   .Name = "Test Irradiance View",
                                                                   .Image = image,
                                                                   .ViewType = ImageViewType::Cube,
                                                                   .ArrayLayers = CubeFaces,
                                                               });
        const Ref<Buffer> staging = Buffer::Create(context, {
                                                                .Name = "Test Irradiance Readback",
                                                                .Size = faceBytes * CubeFaces,
                                                                .Usage = BufferUsage::TransferDst,
                                                            });
        context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                cmd.PrepareForAccess(view, AccessKind::TransferSrc);
                const vk::BufferImageCopy region{
                    .bufferOffset = 0,
                    .bufferRowLength = 0,
                    .bufferImageHeight = 0,
                    .imageSubresource = {.aspectMask = vk::ImageAspectFlagBits::eColor,
                                         .mipLevel = 0,
                                         .baseArrayLayer = 0,
                                         .layerCount = CubeFaces},
                    .imageOffset = {.x = 0, .y = 0, .z = 0},
                    .imageExtent = {.width = faceSize, .height = faceSize, .depth = 1},
                };
                GetVkCommandBuffer(cmd).copyImageToBuffer(GetVkImage(*image),
                                                          vk::ImageLayout::eTransferSrcOptimal,
                                                          GetVkBuffer(*staging), 1, &region);
                cmd.PrepareForAccess(view, AccessKind::SampleGraphics);
            });
        return staging->Download();
    }

    vec3 FaceCenter(const std::vector<u8>& bytes, u32 faceSize, u32 face)
    {
        const auto* halves = reinterpret_cast<const u16*>(bytes.data());
        const u32 x = faceSize / 2;
        const u32 y = faceSize / 2;
        const usize base = ((static_cast<usize>(face) * faceSize + y) * faceSize + x) * 4;
        return vec3(glm::unpackHalf1x16(halves[base + 0]), glm::unpackHalf1x16(halves[base + 1]),
                    glm::unpackHalf1x16(halves[base + 2]));
    }

    // Sweeps a fresh cube-publishing capture over `scene` with the given layer mask, then convolves
    // the published cube through the real IBL path and returns the downloaded irradiance.
    std::vector<u8> CaptureAndConvolve(Context& context, AssetManager& assets, Scene& scene,
                                       u32 layerMask, EnvironmentIbl& ibl)
    {
        const Unique<SceneCapture> capture = SceneCapture::Create({
            .Context = context,
            .Assets = assets,
            .FaceResolution = CaptureFaceSize,
            .Settings = LeanSettings(),
            .Cube = true,
        });

        for (u32 face = 0; face < SceneCapture::FaceCount; ++face)
        {
            capture->SetView({.World = &scene, .VisibleLayers = layerMask});
            context.ImmediateCommands([&](CommandBuffer& cmd) { capture->Render(cmd); });
        }
        REQUIRE(capture->GetCubeRevision() == 1);

        const Ref<ImageView>& cube = capture->GetCubeView();
        REQUIRE(cube != nullptr);
        context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                ibl.EnsureInitialized(cmd);
                cmd.PrepareForAccess(cube, AccessKind::SampleGraphics);
                ibl.GenerateFromCube(cmd, cube, capture->GetCubeFaceSize());
            });
        return DownloadIrradiance(context, ibl);
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "SceneCapture cube: a captured cube feeds IBL and carries the scene's colour "
                  "in the right direction")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(CookPack()).has_value());

    const AssetResult<AssetHandle<MaterialInstance>> green =
        assets.LoadSync<MaterialInstance>(BackdropInstance);
    REQUIRE(green.has_value());

    vector<Ref<Mesh>> meshes;
    const Unique<Scene> scene =
        BuildScene(Context, assets, Types, *green, RenderLayer::Default, meshes);

    const Unique<EnvironmentIbl> ibl = EnvironmentIbl::Create(Context, assets);
    const std::vector<u8> irradiance =
        CaptureAndConvolve(Context, assets, *scene, AllRenderLayers, *ibl);

    const u32 faceSize = EnvironmentIbl::GetIrradianceFaceSize();
    // Face 3 is -Y (toward the green cube), face 2 is +Y (empty). The convolved irradiance must be
    // greatest and green-dominant facing the cube and near black facing away — proving the cube is a
    // valid IBL source, carries the scene colour, and mapped -Y content onto the -Y cube face.
    const vec3 down = FaceCenter(irradiance, faceSize, 3);
    const vec3 up = FaceCenter(irradiance, faceSize, 2);
    CHECK(down.g > 0.1f);
    CHECK(down.g > down.r + 0.05f);
    CHECK(down.g > down.b + 0.05f);
    CHECK(down.g > up.g + 0.05f);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "SceneCapture cube: the revision advances only on a completed six-face sweep")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(CookPack()).has_value());

    const AssetResult<AssetHandle<MaterialInstance>> green =
        assets.LoadSync<MaterialInstance>(BackdropInstance);
    REQUIRE(green.has_value());

    vector<Ref<Mesh>> meshes;
    const Unique<Scene> scene =
        BuildScene(Context, assets, Types, *green, RenderLayer::Default, meshes);

    const Unique<SceneCapture> capture = SceneCapture::Create({
        .Context = Context,
        .Assets = assets,
        .FaceResolution = CaptureFaceSize,
        .Settings = LeanSettings(),
        .Cube = true,
    });
    CHECK(capture->GetCubeRevision() == 0);

    auto pushFace = [&]()
    {
        capture->SetView({.World = scene.get(), .VisibleLayers = AllRenderLayers});
        Context.ImmediateCommands([&](CommandBuffer& cmd) { capture->Render(cmd); });
    };

    // Five faces: an incomplete sweep leaves the revision at zero.
    for (u32 i = 0; i < SceneCapture::FaceCount - 1; ++i)
    {
        pushFace();
    }
    CHECK(capture->GetCubeRevision() == 0);

    // The sixth completes the sweep and advances it once; a second full sweep advances it again.
    pushFace();
    CHECK(capture->GetCubeRevision() == 1);
    for (u32 i = 0; i < SceneCapture::FaceCount; ++i)
    {
        pushFace();
    }
    CHECK(capture->GetCubeRevision() == 2);
}

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "SceneCapture cube: the Environment layer is kept by the shared default and dropped "
    "only when the capture clears it")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(CookPack()).has_value());

    const AssetResult<AssetHandle<MaterialInstance>> green =
        assets.LoadSync<MaterialInstance>(BackdropInstance);
    REQUIRE(green.has_value());

    vector<Ref<Mesh>> meshes;
    const Unique<Scene> scene =
        BuildScene(Context, assets, Types, *green, RenderLayer::Environment, meshes);

    const u32 faceSize = EnvironmentIbl::GetIrradianceFaceSize();

    // The shared capture default keeps the Environment layer, so the green cube reaches the
    // cube and lights the -Y face.
    const Unique<EnvironmentIbl> keptIbl = EnvironmentIbl::Create(Context, assets);
    const vec3 kept = FaceCenter(
        CaptureAndConvolve(Context, assets, *scene, DefaultEnvironmentCaptureLayers, *keptIbl),
        faceSize, 3);

    // A capture that clears the Environment bit drops it, so the same face goes dark.
    const u32 noEnvironment =
        DefaultEnvironmentCaptureLayers & ~RenderLayerBit(RenderLayer::Environment);
    const Unique<EnvironmentIbl> droppedIbl = EnvironmentIbl::Create(Context, assets);
    const vec3 dropped = FaceCenter(
        CaptureAndConvolve(Context, assets, *scene, noEnvironment, *droppedIbl), faceSize, 3);

    CHECK(kept.g > 0.1f);
    CHECK(kept.g > dropped.g + 0.05f);
    CHECK(dropped.g < 0.02f);
}
