// CaptureSurface end-to-end: a scene entity declares a render-to-texture capture the engine drives,
// whose octahedral output a material on the same entity samples. The scene places a bright green
// backdrop directly below the capturing entity — out of the camera's direct view — so the only way
// its color can reach the frame is through the capture: the surface's material samples the capture in
// the -Y direction and displays what the probe saw there. The cases assert:
//
//  - the surface shows the scene-from-here: after a full round-robin refresh, the surface entity
//    renders bright green (the captured backdrop), while the empty frame around it stays black — the
//    color reached the frame only via the capture the component built and drove;
//  - the refresh policy: an EveryFrame capture always reports refreshing, while an OnDemand capture
//    reports refreshing only until its FaceCount-face refresh completes, then idles until MarkDirty;
//  - teardown: a capture built and registered against a component unregisters itself from the
//    drive-list when the component is removed, its entity destroyed, or its scene dropped.
//
// The render runs under the validation gate, so the capture's producer-before-consumer handoff (the
// octahedral map left shader-readable before the surface pass samples it) is validation-clean or the
// gate fails.

#include <doctest/doctest.h>

#include <glm/gtc/packing.hpp>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Primitives.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Renderer/CaptureSurface.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/SceneCapture.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include <gpu/fixture.h>
#include "support/TempPath.h"

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr uvec2 Extent{160, 160};

    // Cooked material ids in the capture_surface fixture pack.
    constexpr AssetId ProbeInstance{0x2451};
    constexpr AssetId BackdropInstance{0x2452};

    vec4 DecodeTexel(const vector<u8>& rgba16f, u32 width, u32 x, u32 y)
    {
        const auto* halves = reinterpret_cast<const u16*>(rgba16f.data());
        const usize base = (static_cast<usize>(y) * width + x) * 4;
        return vec4(glm::unpackHalf1x16(halves[base + 0]), glm::unpackHalf1x16(halves[base + 1]),
                    glm::unpackHalf1x16(halves[base + 2]), glm::unpackHalf1x16(halves[base + 3]));
    }

    // Averages a small block of the downloaded RGBA16F output around a normalized point.
    vec4 SampleBlock(const vector<u8>& pixels, uvec2 extent, vec2 uv, u32 radius = 3)
    {
        const i32 cx = static_cast<i32>(uv.x * static_cast<f32>(extent.x));
        const i32 cy = static_cast<i32>(uv.y * static_cast<f32>(extent.y));
        vec4 sum{0.0f};
        u32 count = 0;
        for (i32 y = cy - static_cast<i32>(radius); y <= cy + static_cast<i32>(radius); ++y)
        {
            for (i32 x = cx - static_cast<i32>(radius); x <= cx + static_cast<i32>(radius); ++x)
            {
                if (x < 0 || y < 0 || x >= static_cast<i32>(extent.x) ||
                    y >= static_cast<i32>(extent.y))
                {
                    continue;
                }
                sum += DecodeTexel(pixels, extent.x, static_cast<u32>(x), static_cast<u32>(y));
                ++count;
            }
        }
        return count > 0 ? sum / static_cast<f32>(count) : vec4(0.0f);
    }

    path CookCapturePack()
    {
        const path fixtureDir = path(GPU_GBUFFER_FIXTURE_DIR);
        const path outArchive = Veng::TestSupport::TempDir() / "veng_capture_surface.vengpack";

        Cook::Cooker cooker;
        Cook::RegisterBuiltinImporters(cooker);
        const VoidResult cookResult =
            cooker.CookPack(fixtureDir / "capture_surface_pack.json", outArchive, {}, nullptr,
                            nullptr, nullptr, nullptr, {}, path(VENG_CORE_SHADER_DIR));
        REQUIRE(cookResult.has_value());
        return outArchive;
    }

    CameraView FrontCamera()
    {
        CameraView camera;
        camera.SetPerspective(glm::radians(45.0f), 1.0f, 0.05f, 1000.0f);
        camera.SetView(vec3(0.0f, 0.0f, 3.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));
        return camera;
    }

    Unique<Viewport> MakeViewport(Context& context, AssetManager& assets)
    {
        // Bloom off so the surface's captured green stays on the surface — a clean empty-corner oracle
        // (the capture-delivered color would otherwise bleed a green halo across the frame).
        SceneRendererSettings settings;
        settings.Bloom = false;
        return Viewport::Create({
            .Context = context,
            .Assets = assets,
            .Region = {.Offset = {0, 0}, .Extent = Extent},
            .ColorFormat = Format::RGBA16Sfloat,
            .Settings = settings,
            .Role = ViewportRole::Offscreen,
        });
    }

    // A scene: a bright green backdrop cube well below the origin (out of the camera's frame, but in
    // the probe's -Y view), and a surface cube at the origin carrying the given capture material and a
    // CaptureSurface of the given refresh policy. Keeps the built meshes alive in `meshes`.
    Unique<Scene> BuildCaptureScene(Context& context, AssetManager& assets, TypeRegistry& types,
                                    const AssetHandle<MaterialInstance>& surfaceMaterial,
                                    const AssetHandle<MaterialInstance>& backdropMaterial,
                                    CaptureRefresh refresh, vector<Ref<Mesh>>& meshes,
                                    Entity& surfaceEntity)
    {
        Unique<Scene> scene = Scene::Create(types);

        const Ref<Mesh> backdrop =
            Mesh::BuildSync(context, Primitives::Cube(6.0f, backdropMaterial), "Capture Backdrop");
        meshes.push_back(backdrop);
        const Entity backdropEntity = scene->CreateEntity();
        scene->Add<Transform>(backdropEntity).Position = vec3(0.0f, -8.0f, 0.0f);
        scene->Add<MeshRenderer>(backdropEntity).Mesh = assets.Adopt(backdrop);

        const Ref<Mesh> surface =
            Mesh::BuildSync(context, Primitives::Cube(1.4f, surfaceMaterial), "Capture Surface");
        meshes.push_back(surface);
        surfaceEntity = scene->CreateEntity();
        scene->Add<Transform>(surfaceEntity);
        scene->Add<MeshRenderer>(surfaceEntity).Mesh = assets.Adopt(surface);

        auto& capture = scene->Add<CaptureSurface>(surfaceEntity);
        capture.Resolution = 128;
        capture.Refresh = refresh;

        return scene;
    }

    // The sibling material of the surface entity (the first material of its MeshRenderer mesh).
    MaterialInstance* SurfaceMaterial(const Scene& scene, Entity entity)
    {
        const auto* mesh = scene.TryGet<MeshRenderer>(entity);
        return mesh->Mesh.Get()->GetMaterials()[0].Get();
    }
}

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "capture surface: the entity's material samples the scene captured from the entity")
{
    RegisterBuiltinTypes(Types);

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(CookCapturePack()).has_value());

    const AssetResult<AssetHandle<MaterialInstance>> probe =
        assets.LoadSync<MaterialInstance>(ProbeInstance);
    const AssetResult<AssetHandle<MaterialInstance>> backdrop =
        assets.LoadSync<MaterialInstance>(BackdropInstance);
    REQUIRE(probe.has_value());
    REQUIRE(backdrop.has_value());

    vector<Ref<Mesh>> meshes;
    Entity surfaceEntity;
    const Unique<Scene> scene =
        BuildCaptureScene(Context, assets, Types, *probe, *backdrop, CaptureRefresh::EveryFrame,
                          meshes, surfaceEntity);

    const Unique<Viewport> viewport = MakeViewport(Context, assets);
    const CaptureSurface& capture = scene->Get<CaptureSurface>(surfaceEntity);
    MaterialInstance* const material = SurfaceMaterial(*scene, surfaceEntity);

    // Drive a full round-robin refresh (one cube face per frame). Each frame builds/pushes the
    // capture from the entity's origin, records the capture ahead of the viewport, and renders the
    // surface sampling the octahedral map — the same producer-before-consumer order the engine uses.
    vector<u8> output;
    for (u32 frame = 0; frame < SceneCapture::FaceCount; ++frame)
    {
        auto* const built = capture.Drive(Context, assets, *scene, vec3(0.0f), material);
        REQUIRE(built != nullptr);
        viewport->SetViewState({.World = scene.get(), .Camera = FrontCamera(), .Delta = 0.016f});
        Context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                built->Render(cmd);
                viewport->Render(cmd);
            });
        output = viewport->GetOutput()->GetImage()->Download();
    }

    // The surface entity fills the frame center: it shows the captured backdrop — bright and clearly
    // green-dominant (the backdrop authors green 4.0, tonemapped near the display ceiling), the color
    // the probe saw in the -Y direction.
    const vec4 center = SampleBlock(output, Extent, vec2(0.5f, 0.5f));
    CHECK(center.g > 0.6f);
    CHECK(center.g > center.r + 0.3f);
    CHECK(center.g > center.b + 0.3f);

    // The frame corner is empty scene: the backdrop sits below the camera's view and the surface fills
    // only the center, so with bloom off the corner carries no green — the captured green reached the
    // center only through the capture, never a direct line of sight.
    const vec4 corner = SampleBlock(output, Extent, vec2(0.08f, 0.08f), 2);
    CHECK(corner.g < 0.2f);
}

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "capture surface: an on-demand capture refreshes once and holds, every-frame does not")
{
    RegisterBuiltinTypes(Types);

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(CookCapturePack()).has_value());

    const AssetResult<AssetHandle<MaterialInstance>> probe =
        assets.LoadSync<MaterialInstance>(ProbeInstance);
    const AssetResult<AssetHandle<MaterialInstance>> backdrop =
        assets.LoadSync<MaterialInstance>(BackdropInstance);
    REQUIRE(probe.has_value());
    REQUIRE(backdrop.has_value());

    SUBCASE("on demand settles after a full refresh and re-arms on MarkDirty")
    {
        vector<Ref<Mesh>> meshes;
        Entity surfaceEntity;
        const Unique<Scene> scene =
            BuildCaptureScene(Context, assets, Types, *probe, *backdrop, CaptureRefresh::OnDemand,
                              meshes, surfaceEntity);
        auto& capture = scene->Get<CaptureSurface>(surfaceEntity);
        MaterialInstance* const material = SurfaceMaterial(*scene, surfaceEntity);

        // While faces are still owed, the capture reports refreshing; each drive pushes one.
        for (u32 frame = 0; frame < SceneCapture::FaceCount; ++frame)
        {
            CHECK(capture.IsRefreshing());
            capture.Drive(Context, assets, *scene, vec3(0.0f), material);
        }

        // Its FaceCount-face refresh complete, an on-demand capture idles — it pushes no more views.
        CHECK_FALSE(capture.IsRefreshing());
        capture.Drive(Context, assets, *scene, vec3(0.0f), material);
        CHECK_FALSE(capture.IsRefreshing());

        // MarkDirty re-arms it for another full refresh.
        capture.MarkDirty();
        CHECK(capture.IsRefreshing());
    }

    SUBCASE("every frame always refreshes")
    {
        vector<Ref<Mesh>> meshes;
        Entity surfaceEntity;
        const Unique<Scene> scene =
            BuildCaptureScene(Context, assets, Types, *probe, *backdrop, CaptureRefresh::EveryFrame,
                              meshes, surfaceEntity);
        const auto& capture = scene->Get<CaptureSurface>(surfaceEntity);
        MaterialInstance* const material = SurfaceMaterial(*scene, surfaceEntity);

        for (u32 frame = 0; frame < SceneCapture::FaceCount * 2; ++frame)
        {
            CHECK(capture.IsRefreshing());
            capture.Drive(Context, assets, *scene, vec3(0.0f), material);
        }
        CHECK(capture.IsRefreshing());
    }
}

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "capture surface: removing the component / entity / scene unregisters the capture")
{
    RegisterBuiltinTypes(Types);

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(CookCapturePack()).has_value());

    const AssetResult<AssetHandle<MaterialInstance>> probe =
        assets.LoadSync<MaterialInstance>(ProbeInstance);
    const AssetResult<AssetHandle<MaterialInstance>> backdrop =
        assets.LoadSync<MaterialInstance>(BackdropInstance);
    REQUIRE(probe.has_value());
    REQUIRE(backdrop.has_value());

    // Mimics the engine drive-list: build the capture, register it (as Application::RegisterCapture
    // does), then tear the owning component down through each of its three lifetimes and assert the
    // capture self-unregisters — the drive-list length restored with no bookkeeping at the sink.
    const auto DriveAndRegister = [&](Scene& scene, Entity entity, vector<SceneCapture*>& driveList)
    {
        const auto& capture = scene.Get<CaptureSurface>(entity);
        auto* const built =
            capture.Drive(Context, assets, scene, vec3(0.0f), SurfaceMaterial(scene, entity));
        REQUIRE(built != nullptr);
        driveList.emplace_back(built);
        built->AttachToDriveList(driveList);
    };

    SUBCASE("removing the component drops the capture")
    {
        vector<Ref<Mesh>> meshes;
        Entity surfaceEntity;
        const Unique<Scene> scene =
            BuildCaptureScene(Context, assets, Types, *probe, *backdrop, CaptureRefresh::EveryFrame,
                              meshes, surfaceEntity);
        vector<SceneCapture*> driveList;
        DriveAndRegister(*scene, surfaceEntity, driveList);
        CHECK(driveList.size() == 1);

        scene->Remove<CaptureSurface>(surfaceEntity);
        CHECK(driveList.empty());
    }

    SUBCASE("destroying the entity drops the capture")
    {
        vector<Ref<Mesh>> meshes;
        Entity surfaceEntity;
        const Unique<Scene> scene =
            BuildCaptureScene(Context, assets, Types, *probe, *backdrop, CaptureRefresh::EveryFrame,
                              meshes, surfaceEntity);
        vector<SceneCapture*> driveList;
        DriveAndRegister(*scene, surfaceEntity, driveList);
        CHECK(driveList.size() == 1);

        scene->DestroyEntity(surfaceEntity);
        CHECK(driveList.empty());
    }

    SUBCASE("dropping the scene drops the capture")
    {
        vector<Ref<Mesh>> meshes;
        Entity surfaceEntity;
        Unique<Scene> scene = BuildCaptureScene(Context, assets, Types, *probe, *backdrop,
                                                CaptureRefresh::EveryFrame, meshes, surfaceEntity);
        vector<SceneCapture*> driveList;
        DriveAndRegister(*scene, surfaceEntity, driveList);
        CHECK(driveList.size() == 1);

        scene.reset();
        CHECK(driveList.empty());
    }
}
