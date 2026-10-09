// A CaptureSurface whose Output is SceneLighting, resolved by the renderer as the scene's image-based
// lighting. The scene is scene_capture_cube.cpp's: a bright green self-lit cube directly below a probe
// at the origin. Each case drives the surface the way a presenting viewport does (CaptureSurface::Drive,
// then SceneCapture::Render) and a standalone SkyResolver the way a SceneRenderer does (Resolve, then
// the pre-graph work), so the resolver's derive count and debug cube read off directly. The cases:
//
//  - cold, then lit: a Sky displaying a baked cube keeps lighting the scene until the capture's first
//    six-face sweep lands, then the capture does, while the skybox still binds the Sky's cube;
//  - once per sweep: an on-demand probe re-lights when a MarkDirty sweep completes, an every-frame
//    one once per six frames — never per frame;
//  - inactive restores: disabling the surface returns the Sky's lighting on the next frame, re-derived
//    to the same maps it lit with before;
//  - any source kind: with no Sky, a direct (per-pixel) Sky and an environment-map Sky the capture
//    still lights, and its irradiance is green toward the cube and dark away from it;
//  - no self-lighting: the capture's own face renderer never lights from a lighting capture.
//
// Skips cleanly (exit 77) on a machine with no Vulkan ICD, like the rest of the gpu band.

#include <array>
#include <cstring>
#include <vector>

#include <doctest/doctest.h>

#include <glm/gtc/packing.hpp>

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/Environment.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Primitives.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Renderer/BakedSkyCube.h>
#include <Veng/Renderer/Buffer.h>
#include <Veng/Renderer/CaptureSurface.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/DescriptorSet.h>
#include <Veng/Renderer/GeneratedTextureService.h>
#include <Veng/Renderer/GraphicsPipeline.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/Native.h>
#include <Veng/Renderer/SceneCapture.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/Types.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include <gpu/fixture.h>
#include "support/TempPath.h"
#include "support/TestCook.h"

#include "Renderer/EnvironmentIbl.h"
#include "Renderer/SkyResolver.h"

#ifdef GPU_GBUFFER_FIXTURE_DIR

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr u32 CubeFaces = 6;
    constexpr u32 ProbeResolution = 32;
    constexpr u32 SourceFaceSize = 64;

    // The green self-lit backdrop instance in the capture fixture pack (Color = (0, 4, 0)).
    constexpr AssetId BackdropInstance{0x2452};
    // The analytic Sky material (radiance 0.5 + 0.5·dir) in the g-buffer fixture pack.
    constexpr AssetId AnalyticSkyInstance{0x00000000000024A1ULL};
    // The uniform panorama this file builds in memory.
    constexpr AssetId PanoramaId{0x70D11933843E4137ULL};

    path CookPack(const char* manifest, const char* archive)
    {
        const path fixtureDir = path(GPU_GBUFFER_FIXTURE_DIR);
        const path outArchive = Veng::TestSupport::TempDir() / archive;
        Cook::Cooker cooker;
        Cook::RegisterBuiltinImporters(cooker);
        const VoidResult cooked = Veng::TestSupport::CookCached(
            cooker, fixtureDir / manifest, outArchive, {}, nullptr, nullptr, nullptr, nullptr, {},
            path(VENG_CORE_SHADER_DIR));
        REQUIRE(cooked.has_value());
        return outArchive;
    }

    // The two fixture packs reuse small hand-assigned ids for different assets, so each mounts
    // into its own manager: the capture scene's, and the one the analytic sky material loads from.
    AssetHandle<MaterialInstance> LoadAnalyticSky(AssetManager& assets)
    {
        REQUIRE(
            assets.Mount(CookPack("gbuffer_pack.json", "veng_gpu_sky_bake.vengpack")).has_value());
        const AssetResult<AssetHandle<MaterialInstance>> material =
            assets.LoadSync<MaterialInstance>(AnalyticSkyInstance);
        REQUIRE(material.has_value());
        return *material;
    }

    AssetHandle<MaterialInstance> LoadBackdrop(AssetManager& assets)
    {
        REQUIRE(
            assets.Mount(CookPack("capture_surface_pack.json", "veng_scene_capture_cube.vengpack"))
                .has_value());
        const AssetResult<AssetHandle<MaterialInstance>> green =
            assets.LoadSync<MaterialInstance>(BackdropInstance);
        REQUIRE(green.has_value());
        return *green;
    }

    // A pack holding one uniform equirectangular panorama, to mount from memory.
    vector<u8> PanoramaArchive(const vec3& colour)
    {
        constexpr u32 Width = 8;
        constexpr u32 Height = 4;
        const CookedEnvironmentHeader header{.Version = CookedEnvironmentVersion,
                                             .Format = static_cast<u32>(Format::RGBA16Sfloat),
                                             .Width = Width,
                                             .Height = Height};
        std::vector<u8> blob(sizeof(header) + static_cast<usize>(Width) * Height * 8);
        std::memcpy(blob.data(), &header, sizeof(header));
        auto* texels = reinterpret_cast<u16*>(blob.data() + sizeof(header));
        for (u32 i = 0; i < Width * Height; ++i)
        {
            texels[i * 4 + 0] = glm::packHalf1x16(colour.r);
            texels[i * 4 + 1] = glm::packHalf1x16(colour.g);
            texels[i * 4 + 2] = glm::packHalf1x16(colour.b);
            texels[i * 4 + 3] = glm::packHalf1x16(1.0f);
        }
        ArchiveWriter writer;
        writer.Add(PanoramaId, AssetTypes::Environment, blob);
        return writer.Build();
    }

    // A real baked radiance cube of the analytic sky, for a Sky displaying a baked cube.
    Ref<BakedSkyCube> BakeSourceCube(Context& context, const EnvironmentIbl& layoutSource,
                                     const MaterialInstance& material)
    {
        const Ref<BakedSkyCube> cube = BakedSkyCube::Create(context, layoutSource.GetSetLayout(),
                                                            Format::RGBA16Sfloat, SourceFaceSize);
        GeneratedTextureService& service = context.GetGeneratedTextures();
        service.SetCostBudget(GeneratedTextureService::UnlimitedCostBudget);
        cube->RequestBake(service, material);
        for (u32 i = 0; i < 8 && cube->IsBakePending(); ++i)
        {
            context.BeginFrame();
            context.EndFrame();
        }
        context.ImmediateCommands([&](CommandBuffer& cmd) { cube->RecordAmortized(cmd); });
        REQUIRE(cube->IsBaked());
        return cube;
    }

    // The green cube directly below the origin, and a lighting probe at the origin.
    Entity BuildScene(Context& context, const AssetManager& assets, Scene& scene,
                      const AssetHandle<MaterialInstance>& green, CaptureRefresh refresh,
                      vector<Ref<Mesh>>& meshes)
    {
        const Ref<Mesh> cube = Mesh::BuildSync(context, Primitives::Cube(6.0f, green), "Backdrop");
        meshes.push_back(cube);
        const Entity backdrop = scene.CreateEntity();
        scene.Add<Transform>(backdrop).Position = vec3(0.0f, -8.0f, 0.0f);
        scene.Add<MeshRenderer>(backdrop).Mesh = assets.Adopt(cube);

        const Entity probe = scene.CreateEntity();
        scene.Add<Transform>(probe);
        auto& surface = scene.Add<CaptureSurface>(probe);
        surface.Output = CaptureOutput::SceneLighting;
        surface.Resolution = ProbeResolution;
        surface.Refresh = refresh;
        return probe;
    }

    // One frame as the engine runs it: the presenting viewport drives the probe (releasing it when
    // disabled, as the compositor's pre-pass does) and the capture renders, then the viewport's
    // renderer resolves the sky and records its pre-graph work.
    struct Frame
    {
        Context& Ctx;
        AssetManager& Assets;
        Scene& World;
        Entity Probe;
        SkyResolver& Resolver;
        Renderer::SceneView& View;

        void operator()() const
        {
            const CaptureSurface& surface = World.Get<CaptureSurface>(Probe);
            if (surface.Enabled)
            {
                SceneCapture* const capture = surface.Drive(Ctx, Assets, World, Probe, 0.0f);
                REQUIRE(capture != nullptr);
                Ctx.ImmediateCommands([&](CommandBuffer& cmd) { capture->Render(cmd); });
            }
            else
            {
                surface.Release();
            }
            Resolver.Resolve(View);
            Ctx.ImmediateCommands(
                [&](CommandBuffer& cmd)
                {
                    Resolver.RecordPreBeginView(cmd, View, Ref<GraphicsPipeline>{});
                    Resolver.RecordPreReplay(cmd, View);
                });
        }
    };

    std::vector<u8> DownloadIrradiance(Context& context, const EnvironmentIbl& ibl)
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

    vec3 FaceCenter(const std::vector<u8>& bytes, u32 face)
    {
        const u32 faceSize = EnvironmentIbl::GetIrradianceFaceSize();
        const auto* halves = reinterpret_cast<const u16*>(bytes.data());
        const u32 c = faceSize / 2;
        const usize base = ((static_cast<usize>(face) * faceSize + c) * faceSize + c) * 4;
        return vec3(glm::unpackHalf1x16(halves[base + 0]), glm::unpackHalf1x16(halves[base + 1]),
                    glm::unpackHalf1x16(halves[base + 2]));
    }

    // Irradiance cube faces: +Y faces away from the green cube, -Y toward it.
    constexpr u32 FaceUp = 2;
    constexpr u32 FaceDown = 3;

    CameraView TestCamera()
    {
        CameraView camera;
        camera.SetPerspective(glm::radians(60.0f), 1.0f, 0.1f, 100.0f);
        camera.SetView(vec3(0.0f), vec3(0.0f, 0.0f, -1.0f), vec3(0.0f, 1.0f, 0.0f));
        return camera;
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "scene lighting capture: a baked Sky lights until the first sweep lands, the "
                  "capture then lights once per sweep, and disabling it restores the Sky")
{
    RegisterBuiltinTypes(Types);
    AssetManager skyAssets(Context, Tasks, Types);
    const AssetHandle<MaterialInstance> skyMaterial = LoadAnalyticSky(skyAssets);
    AssetManager assets(Context, Tasks, Types);
    const AssetHandle<MaterialInstance> green = LoadBackdrop(assets);

    const Unique<EnvironmentIbl> layoutSource = EnvironmentIbl::Create(Context, assets);
    const Ref<BakedSkyCube> sourceCube = BakeSourceCube(Context, *layoutSource, *skyMaterial.Get());

    vector<Ref<Mesh>> meshes;
    const Unique<Scene> scene = Scene::Create(Types);
    const Entity probe =
        BuildScene(Context, assets, *scene, green, CaptureRefresh::OnDemand, meshes);
    Sky& sky = scene->Add<Sky>(scene->CreateEntity());
    static_cast<CubeSky*>(sky.Source.SetActive(TypeIdOf<CubeSky>()))->Cube = sourceCube;
    sky.Lighting = SkyLighting::IBL;

    const CameraView camera = TestCamera();
    const Unique<SkyResolver> resolver = SkyResolver::Create(Context, assets);
    Renderer::SceneView view{.World = *scene, .Camera = camera, .Delta = 0.0f};
    const Frame frame{.Ctx = Context,
                      .Assets = assets,
                      .World = *scene,
                      .Probe = probe,
                      .Resolver = *resolver,
                      .View = view};

    // Cold: five faces of the first sweep leave the Sky lighting the scene.
    for (u32 i = 0; i < SceneCapture::FaceCount - 1; ++i)
    {
        frame();
    }
    CHECK(resolver->GetLightingDeriveCount() == 0);
    CHECK_FALSE(resolver->IsLightingCubeResolved());
    CHECK(resolver->GetLightingDebugCube() == sourceCube->GetCubeView());
    const std::vector<u8> skyIrradiance = DownloadIrradiance(Context, resolver->GetIbl());

    // The sixth lands the sweep: the capture lights the scene, and the skybox still shows the Sky.
    frame();
    const SceneCapture* const capture = scene->Get<CaptureSurface>(probe).GetCapture();
    REQUIRE(capture != nullptr);
    CHECK(resolver->GetLightingDeriveCount() == 1);
    CHECK(resolver->IsLightingCubeResolved());
    CHECK(resolver->GetLightingDebugCube() == capture->GetCubeView());
    CHECK(resolver->GetSkyConsumerSet().get() == sourceCube->GetSet().get());
    CHECK(DownloadIrradiance(Context, resolver->GetIbl()) != skyIrradiance);

    // A settled on-demand probe completes no sweep, so it is not re-derived.
    for (u32 i = 0; i < 3; ++i)
    {
        frame();
    }
    CHECK(resolver->GetLightingDeriveCount() == 1);

    // MarkDirty re-arms it: nothing moves until that sweep completes, then the scene re-lights once.
    scene->Get<CaptureSurface>(probe).MarkDirty();
    for (u32 i = 0; i < SceneCapture::FaceCount - 1; ++i)
    {
        frame();
    }
    CHECK(resolver->GetLightingDeriveCount() == 1);
    frame();
    CHECK(resolver->GetLightingDeriveCount() == 2);

    // Disabled, it lets go on the next frame, and the Sky's lighting is derived again exactly.
    scene->Get<CaptureSurface>(probe).Enabled = false;
    frame();
    CHECK_FALSE(resolver->IsLightingCubeResolved());
    CHECK(resolver->GetLightingDebugCube() == sourceCube->GetCubeView());
    CHECK(resolver->GetLightingDeriveCount() == 2);
    CHECK(DownloadIrradiance(Context, resolver->GetIbl()) == skyIrradiance);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "scene lighting capture: with no Sky the capture lights the scene from the "
                  "direction it saw, once per six-face sweep, and never lights its own faces")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const AssetHandle<MaterialInstance> green = LoadBackdrop(assets);

    vector<Ref<Mesh>> meshes;
    const Unique<Scene> scene = Scene::Create(Types);
    const Entity probe =
        BuildScene(Context, assets, *scene, green, CaptureRefresh::EveryFrame, meshes);

    const CameraView camera = TestCamera();
    const Unique<SkyResolver> resolver = SkyResolver::Create(Context, assets);
    Renderer::SceneView view{.World = *scene, .Camera = camera, .Delta = 0.0f};
    const Frame frame{.Ctx = Context,
                      .Assets = assets,
                      .World = *scene,
                      .Probe = probe,
                      .Resolver = *resolver,
                      .View = view};

    for (u32 i = 0; i < SceneCapture::FaceCount; ++i)
    {
        frame();
    }
    const SceneCapture* const capture = scene->Get<CaptureSurface>(probe).GetCapture();
    REQUIRE(capture != nullptr);
    CHECK(resolver->GetLightingDeriveCount() == 1);
    CHECK(resolver->GetResolvedLighting() == SkyLighting::IBL);
    CHECK(resolver->IsIblSourceResident(view));
    CHECK(resolver->GetLightingDebugCube() == capture->GetCubeView());

    // It lights from where things are: green toward the cube below, near black above.
    const std::vector<u8> irradiance = DownloadIrradiance(Context, resolver->GetIbl());
    const vec3 down = FaceCenter(irradiance, FaceDown);
    const vec3 up = FaceCenter(irradiance, FaceUp);
    CHECK(down.g > 0.1f);
    CHECK(down.g > down.r + 0.05f);
    CHECK(down.g > down.b + 0.05f);
    CHECK(up.g < 0.05f);

    // An every-frame probe re-convolves once per completed sweep, not per frame.
    for (u32 i = 0; i < SceneCapture::FaceCount - 1; ++i)
    {
        frame();
    }
    CHECK(resolver->GetLightingDeriveCount() == 1);
    frame();
    CHECK(resolver->GetLightingDeriveCount() == 2);

    // Its own faces were lit without it, through both sweeps.
    CHECK(capture->GetFaceRenderer().GetLightingDeriveCount() == 0);

    // And a capture face resolving the same scene ignores it.
    const Unique<SkyResolver> faceResolver = SkyResolver::Create(Context, assets);
    Renderer::SceneView faceView{.World = *scene, .Camera = camera, .CaptureFace = true};
    faceResolver->Resolve(faceView);
    CHECK_FALSE(faceResolver->IsLightingCubeResolved());
    CHECK(faceResolver->GetResolvedLighting() == SkyLighting::None);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "scene lighting capture: a direct Sky, which cannot light, is lit by the capture")
{
    RegisterBuiltinTypes(Types);
    AssetManager skyAssets(Context, Tasks, Types);
    const AssetHandle<MaterialInstance> skyMaterial = LoadAnalyticSky(skyAssets);
    AssetManager assets(Context, Tasks, Types);
    const AssetHandle<MaterialInstance> green = LoadBackdrop(assets);

    vector<Ref<Mesh>> meshes;
    const Unique<Scene> scene = Scene::Create(Types);
    const Entity probe =
        BuildScene(Context, assets, *scene, green, CaptureRefresh::OnDemand, meshes);
    Sky& sky = scene->Add<Sky>(scene->CreateEntity());
    auto* source = static_cast<MaterialSky*>(sky.Source.SetActive(TypeIdOf<MaterialSky>()));
    source->Material = skyMaterial;
    source->Mode = SkyMode::Direct;
    sky.Lighting = SkyLighting::SH;

    const CameraView camera = TestCamera();
    const Unique<SkyResolver> resolver = SkyResolver::Create(Context, assets);
    Renderer::SceneView view{.World = *scene, .Camera = camera, .Delta = 0.0f};
    const Frame frame{.Ctx = Context,
                      .Assets = assets,
                      .World = *scene,
                      .Probe = probe,
                      .Resolver = *resolver,
                      .View = view};

    frame();
    CHECK(resolver->GetResolvedLighting() == SkyLighting::None);
    for (u32 i = 1; i < SceneCapture::FaceCount; ++i)
    {
        frame();
    }
    CHECK(resolver->GetResolvedKind() == SkySourceKind::Material);
    CHECK(resolver->GetResolvedLighting() == SkyLighting::IBL);
    CHECK(resolver->GetLightingDeriveCount() == 1);
    CHECK(FaceCenter(DownloadIrradiance(Context, resolver->GetIbl()), FaceDown).g > 0.1f);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "scene lighting capture: an environment Sky's lighting yields to the capture "
                  "and is convolved again when it goes")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const AssetHandle<MaterialInstance> green = LoadBackdrop(assets);
    const MountHandle mount =
        assets.MountMemory(PanoramaArchive(vec3(0.02f, 0.02f, 0.6f)), "uniform panorama");
    const AssetResult<AssetHandle<EnvironmentMap>> panorama =
        assets.LoadSync<EnvironmentMap>(PanoramaId);
    REQUIRE(panorama.has_value());

    vector<Ref<Mesh>> meshes;
    const Unique<Scene> scene = Scene::Create(Types);
    const Entity probe =
        BuildScene(Context, assets, *scene, green, CaptureRefresh::OnDemand, meshes);
    Sky& sky = scene->Add<Sky>(scene->CreateEntity());
    static_cast<EnvironmentSky*>(sky.Source.SetActive(TypeIdOf<EnvironmentSky>()))->Map = *panorama;
    sky.Lighting = SkyLighting::IBL;

    const CameraView camera = TestCamera();
    const Unique<SkyResolver> resolver = SkyResolver::Create(Context, assets);
    Renderer::SceneView view{.World = *scene, .Camera = camera, .Delta = 0.0f};
    const Frame frame{.Ctx = Context,
                      .Assets = assets,
                      .World = *scene,
                      .Probe = probe,
                      .Resolver = *resolver,
                      .View = view};

    for (u32 i = 0; i < SceneCapture::FaceCount - 1; ++i)
    {
        frame();
    }
    CHECK(resolver->GetLightingDeriveCount() == 0);
    const std::vector<u8> skyIrradiance = DownloadIrradiance(Context, resolver->GetIbl());
    // The uniform blue panorama lights every direction blue.
    CHECK(FaceCenter(skyIrradiance, FaceDown).b > FaceCenter(skyIrradiance, FaceDown).g);

    // The sweep lands: the capture's green cube now lights from below, and the panorama's own
    // regeneration does not overwrite it on later frames.
    for (u32 i = 0; i < 3; ++i)
    {
        frame();
    }
    CHECK(resolver->GetLightingDeriveCount() == 1);
    const vec3 litDown = FaceCenter(DownloadIrradiance(Context, resolver->GetIbl()), FaceDown);
    CHECK(litDown.g > FaceCenter(skyIrradiance, FaceDown).g + 0.05f);

    // Gone, the panorama's lighting is convolved again, exactly.
    scene->Get<CaptureSurface>(probe).Enabled = false;
    frame();
    CHECK_FALSE(resolver->IsLightingCubeResolved());
    CHECK(DownloadIrradiance(Context, resolver->GetIbl()) == skyIrradiance);
}

#endif
