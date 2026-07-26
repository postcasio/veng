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
//  - the published probe centre: with CenterSlot named, the drive writes the world position it
//    rendered from into the material's centre field with a validity flag, which a
//    parallax-correcting consumer needs and cannot derive from its fragment inputs;
//  - teardown: a capture built and registered against a component unregisters itself from the
//    drive-list when the component is removed, its entity destroyed, or its scene dropped — and the
//    material it fed stops sampling the capture rather than freezing on a released bindless slot.
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
    // A probe material whose capture slots carry descriptive, non-default names (CaptureMap /
    // CaptureSampler), for the configurable-slot case.
    constexpr AssetId NamedProbeInstance{0x2471};
    // An opaque, depth-writing, double-sided probe material with an authored sample Direction, for
    // the exclusion cases: it both contributes to and occludes a capture centered on it.
    constexpr AssetId ShellInstance{0x2491};
    // A bright red unlit marker, the second mesh the exclusion cases look for in the map.
    constexpr AssetId MarkerInstance{0x2492};
    // A probe material that reads the published centre: it falls back to bright magenta on a zero
    // validity flag, and can display the centre itself instead of the capture (its Display param).
    constexpr AssetId ParallaxInstance{0x24B1};

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

    // The sibling material of the surface entity (the first material of its MeshRenderer mesh) — the
    // resident handle Drive takes, so the component can hold it while it has slots bound on it.
    AssetHandle<MaterialInstance> SurfaceMaterial(const Scene& scene, Entity entity)
    {
        const auto* mesh = scene.TryGet<MeshRenderer>(entity);
        return mesh->Mesh.Get()->GetMaterials()[0];
    }

    // A scene for the exclusion cases: two meshes in distinct directions from the origin — a green
    // one straight below (-Y) and a red one straight ahead (+Z), both far outside the camera's frame
    // — and, at the origin, a small double-sided *opaque* shell carrying the capture material and a
    // CaptureSurface. The shell stands between a probe at the origin and everything else in every
    // direction, which is what a pane, canopy or monitor does to a probe sitting on its own surface,
    // generalized so one scene exercises every direction. Because the shell is opaque it writes
    // depth, so it hides the geometry beyond it even if it were dropped from color alone.
    Unique<Scene> BuildExclusionScene(Context& context, AssetManager& assets, TypeRegistry& types,
                                      const AssetHandle<MaterialInstance>& shellMaterial,
                                      const AssetHandle<MaterialInstance>& belowMaterial,
                                      const AssetHandle<MaterialInstance>& aheadMaterial,
                                      vector<Ref<Mesh>>& meshes, Entity& shellEntity)
    {
        Unique<Scene> scene = Scene::Create(types);

        const auto place = [&](const AssetHandle<MaterialInstance>& material, const char* name,
                               const vec3& position)
        {
            const Ref<Mesh> mesh = Mesh::BuildSync(context, Primitives::Cube(4.0f, material), name);
            meshes.push_back(mesh);
            const Entity entity = scene->CreateEntity();
            scene->Add<Transform>(entity).Position = position;
            scene->Add<MeshRenderer>(entity).Mesh = assets.Adopt(mesh);
        };
        place(belowMaterial, "Exclusion Below", vec3(0.0f, -8.0f, 0.0f));
        place(aheadMaterial, "Exclusion Ahead", vec3(0.0f, 0.0f, 12.0f));

        const Ref<Mesh> shell =
            Mesh::BuildSync(context, Primitives::Cube(1.4f, shellMaterial), "Exclusion Shell");
        meshes.push_back(shell);
        shellEntity = scene->CreateEntity();
        scene->Add<Transform>(shellEntity);
        scene->Add<MeshRenderer>(shellEntity).Mesh = assets.Adopt(shell);

        auto& capture = scene->Add<CaptureSurface>(shellEntity);
        capture.Resolution = 128;
        capture.Refresh = CaptureRefresh::EveryFrame;

        return scene;
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
    const AssetHandle<MaterialInstance> material = SurfaceMaterial(*scene, surfaceEntity);

    // Drive a full round-robin refresh (one cube face per frame). Each frame builds/pushes the
    // capture from the entity's origin, records the capture ahead of the viewport, and renders the
    // surface sampling the octahedral map — the same producer-before-consumer order the engine uses.
    vector<u8> output;
    for (u32 frame = 0; frame < SceneCapture::FaceCount; ++frame)
    {
        auto* const built =
            capture.Drive(Context, assets, *scene, surfaceEntity, vec3(0.0f), material);
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

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "capture surface: the output binds onto the configured non-default slot names")
{
    RegisterBuiltinTypes(Types);

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(CookCapturePack()).has_value());

    // The named-slot probe declares CaptureMap / CaptureSampler, not the default Texture / Sampler.
    const AssetResult<AssetHandle<MaterialInstance>> named =
        assets.LoadSync<MaterialInstance>(NamedProbeInstance);
    const AssetResult<AssetHandle<MaterialInstance>> backdrop =
        assets.LoadSync<MaterialInstance>(BackdropInstance);
    REQUIRE(named.has_value());
    REQUIRE(backdrop.has_value());

    vector<Ref<Mesh>> meshes;
    Entity surfaceEntity;
    const Unique<Scene> scene =
        BuildCaptureScene(Context, assets, Types, *named, *backdrop, CaptureRefresh::EveryFrame,
                          meshes, surfaceEntity);

    // Author the descriptive slot names on the component; Drive must bind the capture output onto
    // these rather than the hardcoded Texture / Sampler, or the material samples nothing.
    auto& capture = scene->Get<CaptureSurface>(surfaceEntity);
    capture.TextureSlot = "CaptureMap";
    capture.SamplerSlot = "CaptureSampler";
    const AssetHandle<MaterialInstance> material = SurfaceMaterial(*scene, surfaceEntity);

    const Unique<Viewport> viewport = MakeViewport(Context, assets);

    vector<u8> output;
    for (u32 frame = 0; frame < SceneCapture::FaceCount; ++frame)
    {
        auto* const built =
            capture.Drive(Context, assets, *scene, surfaceEntity, vec3(0.0f), material);
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

    // The center shows the captured backdrop's green only if the output reached the material through
    // the CaptureMap / CaptureSampler slots — the non-default names bound end to end.
    const vec4 center = SampleBlock(output, Extent, vec2(0.5f, 0.5f));
    CHECK(center.g > 0.6f);
    CHECK(center.g > center.r + 0.3f);
    CHECK(center.g > center.b + 0.3f);
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
        const AssetHandle<MaterialInstance> material = SurfaceMaterial(*scene, surfaceEntity);

        // While faces are still owed, the capture reports refreshing; each drive pushes one.
        for (u32 frame = 0; frame < SceneCapture::FaceCount; ++frame)
        {
            CHECK(capture.IsRefreshing());
            capture.Drive(Context, assets, *scene, surfaceEntity, vec3(0.0f), material);
        }

        // Its FaceCount-face refresh complete, an on-demand capture idles — it pushes no more views.
        CHECK_FALSE(capture.IsRefreshing());
        capture.Drive(Context, assets, *scene, surfaceEntity, vec3(0.0f), material);
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
        const AssetHandle<MaterialInstance> material = SurfaceMaterial(*scene, surfaceEntity);

        for (u32 frame = 0; frame < SceneCapture::FaceCount * 2; ++frame)
        {
            CHECK(capture.IsRefreshing());
            capture.Drive(Context, assets, *scene, surfaceEntity, vec3(0.0f), material);
        }
        CHECK(capture.IsRefreshing());
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "capture surface: the drive publishes the probe centre and its validity flag")
{
    RegisterBuiltinTypes(Types);

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(CookCapturePack()).has_value());

    const AssetResult<AssetHandle<MaterialInstance>> parallax =
        assets.LoadSync<MaterialInstance>(ParallaxInstance);
    const AssetResult<AssetHandle<MaterialInstance>> backdrop =
        assets.LoadSync<MaterialInstance>(BackdropInstance);
    REQUIRE(parallax.has_value());
    REQUIRE(backdrop.has_value());

    vector<Ref<Mesh>> meshes;
    Entity surfaceEntity;
    const Unique<Scene> scene =
        BuildCaptureScene(Context, assets, Types, *parallax, *backdrop, CaptureRefresh::EveryFrame,
                          meshes, surfaceEntity);
    auto& capture = scene->Get<CaptureSurface>(surfaceEntity);
    capture.CenterSlot = "Center";
    const AssetHandle<MaterialInstance> material = SurfaceMaterial(*scene, surfaceEntity);
    const Unique<Viewport> viewport = MakeViewport(Context, assets);

    // Renders one frame (recording @p built's capture ahead of it when one drove) and returns the
    // frame center — which the surface entity fills, so the center *is* what the material produced.
    const auto RenderFrame = [&](SceneCapture* built)
    {
        viewport->SetViewState({.World = scene.get(), .Camera = FrontCamera(), .Delta = 0.016f});
        Context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                if (built != nullptr)
                {
                    built->Render(cmd);
                }
                viewport->Render(cmd);
            });
        const vector<u8> output = viewport->GetOutput()->GetImage()->Download();
        return SampleBlock(output, Extent, vec2(0.5f, 0.5f));
    };

    // Before any drive the material's cooked centre is zero, so its validity flag is clear and the
    // fragment takes its magenta fallback. That the branch is reachable at all is the point of the
    // flag: it is what distinguishes an unpopulated handle slot from a probe at the world origin.
    material->SetParam("Display", vec4(0.0f));
    const vec4 unbound = RenderFrame(nullptr);
    CHECK(unbound.r > 0.4f);
    CHECK(unbound.b > 0.4f);
    CHECK(unbound.g < 0.15f);

    // Driving from a named world position publishes it: with Display set, the surface renders the
    // published centre as radiance. Three axes in turn, so the value is shown to be the position
    // handed to Drive rather than a constant, the origin, or the entity's own transform.
    material->SetParam("Display", vec4(1.0f, 0.0f, 0.0f, 0.0f));

    const vec4 up = RenderFrame(
        capture.Drive(Context, assets, *scene, surfaceEntity, vec3(0.0f, 4.0f, 0.0f), material));
    CHECK(up.g > 0.6f);
    CHECK(up.g > up.r + 0.3f);
    CHECK(up.g > up.b + 0.3f);

    const vec4 right = RenderFrame(
        capture.Drive(Context, assets, *scene, surfaceEntity, vec3(4.0f, 0.0f, 0.0f), material));
    CHECK(right.r > 0.6f);
    CHECK(right.r > right.g + 0.3f);
    CHECK(right.r > right.b + 0.3f);

    const vec4 forward = RenderFrame(
        capture.Drive(Context, assets, *scene, surfaceEntity, vec3(0.0f, 0.0f, 4.0f), material));
    CHECK(forward.b > 0.6f);
    CHECK(forward.b > forward.r + 0.3f);
    CHECK(forward.b > forward.g + 0.3f);

    // An empty CenterSlot publishes nothing, so the last centre stands: the slot is opt-in, and a
    // material that only samples by direction declares no such field for the drive to find.
    capture.CenterSlot.clear();
    const vec4 unpublished = RenderFrame(
        capture.Drive(Context, assets, *scene, surfaceEntity, vec3(0.0f, 4.0f, 0.0f), material));
    CHECK(unpublished.b > 0.6f);
    CHECK(unpublished.b > unpublished.g + 0.3f);
}

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "capture surface: teardown unbinds, so the material stops sampling the released capture")
{
    RegisterBuiltinTypes(Types);

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(CookCapturePack()).has_value());

    const AssetResult<AssetHandle<MaterialInstance>> parallax =
        assets.LoadSync<MaterialInstance>(ParallaxInstance);
    const AssetResult<AssetHandle<MaterialInstance>> backdrop =
        assets.LoadSync<MaterialInstance>(BackdropInstance);
    REQUIRE(parallax.has_value());
    REQUIRE(backdrop.has_value());

    // Drives a full refresh (so the surface is showing the captured backdrop), tears the binding down
    // the requested way, and renders one more frame with nothing driving the capture.
    const auto RunTeardown = [&](const bool removeComponent)
    {
        vector<Ref<Mesh>> meshes;
        Entity surfaceEntity;
        const Unique<Scene> scene =
            BuildCaptureScene(Context, assets, Types, *parallax, *backdrop,
                              CaptureRefresh::EveryFrame, meshes, surfaceEntity);
        auto& capture = scene->Get<CaptureSurface>(surfaceEntity);
        capture.CenterSlot = "Center";
        const AssetHandle<MaterialInstance> material = SurfaceMaterial(*scene, surfaceEntity);
        material->SetParam("Display", vec4(0.0f));
        const Unique<Viewport> viewport = MakeViewport(Context, assets);

        const auto RenderFrame = [&](SceneCapture* built)
        {
            viewport->SetViewState(
                {.World = scene.get(), .Camera = FrontCamera(), .Delta = 0.016f});
            Context.ImmediateCommands(
                [&](CommandBuffer& cmd)
                {
                    if (built != nullptr)
                    {
                        built->Render(cmd);
                    }
                    viewport->Render(cmd);
                });
            const vector<u8> output = viewport->GetOutput()->GetImage()->Download();
            return SampleBlock(output, Extent, vec2(0.5f, 0.5f));
        };

        vec4 captured{0.0f};
        for (u32 frame = 0; frame < SceneCapture::FaceCount; ++frame)
        {
            auto* const built =
                capture.Drive(Context, assets, *scene, surfaceEntity, vec3(0.0f), material);
            REQUIRE(built != nullptr);
            captured = RenderFrame(built);
        }
        CHECK(captured.g > 0.6f);
        CHECK(captured.g > captured.r + 0.3f);
        CHECK(captured.g > captured.b + 0.3f);

        if (removeComponent)
        {
            scene->Remove<CaptureSurface>(surfaceEntity);
        }
        else
        {
            capture.Unbind();
        }

        // With the slots cleared the validity flag reads 0 and the fragment takes its fallback. A
        // teardown that left them bound renders this frame identically to the one above, because the
        // material still names the bindless slot the capture's release handed back to the free list.
        const vec4 reverted = RenderFrame(nullptr);
        CHECK(reverted.r > 0.4f);
        CHECK(reverted.b > 0.4f);
        CHECK(reverted.g < 0.15f);
    };

    SUBCASE("removing the component clears what its drive bound")
    {
        RunTeardown(true);
    }

    SUBCASE("an explicit Unbind clears it while the component lives on")
    {
        RunTeardown(false);
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
        auto* const built = capture.Drive(Context, assets, scene, entity, vec3(0.0f),
                                          SurfaceMaterial(scene, entity));
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

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "capture exclusion: a capture never draws the mesh it feeds")
{
    RegisterBuiltinTypes(Types);

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(CookCapturePack()).has_value());

    const AssetResult<AssetHandle<MaterialInstance>> shell =
        assets.LoadSync<MaterialInstance>(ShellInstance);
    const AssetResult<AssetHandle<MaterialInstance>> below =
        assets.LoadSync<MaterialInstance>(BackdropInstance);
    const AssetResult<AssetHandle<MaterialInstance>> ahead =
        assets.LoadSync<MaterialInstance>(MarkerInstance);
    REQUIRE(shell.has_value());
    REQUIRE(below.has_value());
    REQUIRE(ahead.has_value());

    vector<Ref<Mesh>> meshes;
    Entity shellEntity;
    const Unique<Scene> scene =
        BuildExclusionScene(Context, assets, Types, *shell, *below, *ahead, meshes, shellEntity);
    const CaptureSurface& capture = scene->Get<CaptureSurface>(shellEntity);
    const AssetHandle<MaterialInstance> material = SurfaceMaterial(*scene, shellEntity);
    const Unique<Viewport> viewport = MakeViewport(Context, assets);

    // Drives a full round-robin refresh from `probe` with the shell displaying the capture in
    // `direction`, and returns the frame center — which is the shell, so the center *is* the
    // capture's content in that direction.
    const auto SampleCapturedDirection = [&](const vec3& probe, const vec3& direction)
    {
        vector<u8> output;
        for (u32 frame = 0; frame < SceneCapture::FaceCount; ++frame)
        {
            auto* const built =
                capture.Drive(Context, assets, *scene, shellEntity, probe, material);
            REQUIRE(built != nullptr);
            material->SetParam("Direction", vec4(direction, 0.0f));
            viewport->SetViewState(
                {.World = scene.get(), .Camera = FrontCamera(), .Delta = 0.016f});
            Context.ImmediateCommands(
                [&](CommandBuffer& cmd)
                {
                    built->Render(cmd);
                    viewport->Render(cmd);
                });
            output = viewport->GetOutput()->GetImage()->Download();
        }
        return SampleBlock(output, Extent, vec2(0.5f, 0.5f));
    };

    // Both probe placements sit within the shell's own geometry, which is the whole point: a probe on
    // its own surface is what a pane or monitor produces, and CaptureView::Near (0.05) is far too
    // small to hide a surface 0.7 or 1.4 units away.
    SUBCASE("the probe at the entity's own position, inside its own surface")
    {
        // Straight down: the green mesh is 8 units below, with the shell's own bottom face 0.7 units
        // into the ray. The green reaching the map is the shell being absent from color *and* depth —
        // an opaque shell left in depth would reject the mesh behind it just as thoroughly.
        const vec4 down = SampleCapturedDirection(vec3(0.0f), vec3(0.0f, -1.0f, 0.0f));
        CHECK(down.g > 0.5f);
        CHECK(down.g > down.r + 0.2f);
        CHECK(down.g > down.b + 0.2f);

        // Straight ahead: the red mesh 12 units out. A second entity in a second direction, so the
        // capture is shown to drop the one nominated entity rather than the scene's other meshes.
        const vec4 forward = SampleCapturedDirection(vec3(0.0f), vec3(0.0f, 0.0f, 1.0f));
        CHECK(forward.r > 0.5f);
        CHECK(forward.r > forward.g + 0.2f);
        CHECK(forward.r > forward.b + 0.2f);
    }

    SUBCASE("the probe on its own mesh's surface")
    {
        // On the shell's top face (the 1.4 cube's +Y plane), the case the plane of a pane produces:
        // looking down, the shell's opposite face is 1.4 units into the ray. The probe samples the
        // geometry beyond it, not its own back face.
        const vec4 down = SampleCapturedDirection(vec3(0.0f, 0.7f, 0.0f), vec3(0.0f, -1.0f, 0.0f));
        CHECK(down.g > 0.5f);
        CHECK(down.g > down.r + 0.2f);
        CHECK(down.g > down.b + 0.2f);
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "capture exclusion: CaptureView::Exclude is the whole mechanism, and its default "
                  "excludes nothing")
{
    RegisterBuiltinTypes(Types);

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(CookCapturePack()).has_value());

    const AssetResult<AssetHandle<MaterialInstance>> shell =
        assets.LoadSync<MaterialInstance>(ShellInstance);
    const AssetResult<AssetHandle<MaterialInstance>> below =
        assets.LoadSync<MaterialInstance>(BackdropInstance);
    const AssetResult<AssetHandle<MaterialInstance>> ahead =
        assets.LoadSync<MaterialInstance>(MarkerInstance);
    REQUIRE(shell.has_value());
    REQUIRE(below.has_value());
    REQUIRE(ahead.has_value());

    vector<Ref<Mesh>> meshes;
    Entity shellEntity;
    const Unique<Scene> scene =
        BuildExclusionScene(Context, assets, Types, *shell, *below, *ahead, meshes, shellEntity);
    const CaptureSurface& capture = scene->Get<CaptureSurface>(shellEntity);
    const AssetHandle<MaterialInstance> material = SurfaceMaterial(*scene, shellEntity);
    const Unique<Viewport> viewport = MakeViewport(Context, assets);

    // Drives the component (which binds the output onto the material), then overwrites the pushed
    // source with an explicit CaptureView so the two runs differ in exactly one field.
    const auto SampleWithExclusion = [&](const Entity exclude)
    {
        vector<u8> output;
        for (u32 frame = 0; frame < SceneCapture::FaceCount; ++frame)
        {
            auto* const built =
                capture.Drive(Context, assets, *scene, shellEntity, vec3(0.0f), material);
            REQUIRE(built != nullptr);
            built->SetView({.World = scene.get(), .Position = vec3(0.0f), .Exclude = exclude});
            material->SetParam("Direction", vec4(0.0f, -1.0f, 0.0f, 0.0f));
            viewport->SetViewState(
                {.World = scene.get(), .Camera = FrontCamera(), .Delta = 0.016f});
            Context.ImmediateCommands(
                [&](CommandBuffer& cmd)
                {
                    built->Render(cmd);
                    viewport->Render(cmd);
                });
            output = viewport->GetOutput()->GetImage()->Download();
        }
        return SampleBlock(output, Extent, vec2(0.5f, 0.5f));
    };

    // The default: a capture that excludes nothing renders the scene whole, so the shell occludes the
    // green mesh from a probe inside it and no green reaches the map.
    const vec4 unexcluded = SampleWithExclusion(Entity::Null);
    CHECK(unexcluded.g < 0.2f);

    // Naming the entity is the whole difference: the same scene, the same probe, one field.
    const vec4 excluded = SampleWithExclusion(shellEntity);
    CHECK(excluded.g > 0.5f);
    CHECK(excluded.g > excluded.r + 0.2f);
}
