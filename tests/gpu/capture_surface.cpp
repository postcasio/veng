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
//  - the published capture frame: with OrientationSlot named, the drive writes the basis the faces
//    were oriented by as a quaternion — the identity for a world-aligned capture, the carrier's own
//    rotation for an entity-aligned one — and teardown returns it to the identity;
//  - teardown: a capture built and registered against a component unregisters itself from the
//    drive-list when the component is removed, its entity destroyed, or its scene dropped — and the
//    material it fed stops sampling the capture rather than freezing on a released bindless slot;
//  - slot ownership: a surface built, driven and dropped leaves the bindless registry's free counts
//    where it found them, so a consumer building and dropping scenes over a run holds a steady count
//    rather than walking each array's fixed capacity down to its fatal exhaustion.
//
// The render runs under the validation gate, so the capture's producer-before-consumer handoff (the
// octahedral map left shader-readable before the surface pass samples it) is validation-clean or the
// gate fails.

#include <doctest/doctest.h>

#include <glm/gtc/matrix_transform.hpp>
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
#include <Veng/Renderer/ViewportCompositor.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SystemRegistry.h>
#include <Veng/World.h>
#include <Veng/WorldRunner.h>

#include <utility>

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
    // Opaque, depth-writing wall materials (green / red), for the box-distance and registration cases.
    constexpr AssetId WallGreenInstance{0x24D1};
    constexpr AssetId WallRedInstance{0x24D2};
    // A material that samples the octahedral distance map in an authored Direction and returns the
    // recorded distance as radiance, for the opt-in end-to-end case.
    constexpr AssetId DistanceProbeInstance{0x24F1};

    // The octahedral UV of a direction — a CPU mirror of Veng/octahedral.slang's OctahedralUV, so a
    // test can address the distance map's texel for a chosen world direction.
    vec2 OctahedralUv(const vec3& dir)
    {
        const vec3 a = dir / (std::abs(dir.x) + std::abs(dir.y) + std::abs(dir.z));
        vec2 uv;
        if (a.z >= 0.0f)
        {
            uv = vec2(a.x, a.y);
        }
        else
        {
            uv = vec2((1.0f - std::abs(a.y)) * (a.x >= 0.0f ? 1.0f : -1.0f),
                      (1.0f - std::abs(a.x)) * (a.y >= 0.0f ? 1.0f : -1.0f));
        }
        return uv * 0.5f + 0.5f;
    }

    // Reads the single-channel R32Sfloat octahedral distance map at the texel a direction addresses.
    f32 DistanceInDirection(const vector<u8>& r32, u32 edge, const vec3& dir)
    {
        const vec2 uv = OctahedralUv(glm::normalize(dir));
        const i32 x = glm::clamp(static_cast<i32>(uv.x * static_cast<f32>(edge)), 0,
                                 static_cast<i32>(edge) - 1);
        const i32 y = glm::clamp(static_cast<i32>(uv.y * static_cast<f32>(edge)), 0,
                                 static_cast<i32>(edge) - 1);
        const auto* floats = reinterpret_cast<const f32*>(r32.data());
        return floats[static_cast<usize>(y) * edge + static_cast<usize>(x)];
    }

    // The radial distance from the origin to the surface of an axis-aligned box of half-extent
    // `halfExtent` along `dir` — the analytic answer a capture centred in the box must record.
    f32 BoxRadialDistance(f32 halfExtent, const vec3& dir)
    {
        const vec3 n = glm::normalize(dir);
        return halfExtent / glm::max(glm::max(std::abs(n.x), std::abs(n.y)), std::abs(n.z));
    }

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

    // Adds one opaque, depth-writing wall cube of the given edge, centred at `center`.
    void AddWall(Scene& scene, AssetManager& assets, vector<Ref<Mesh>>& meshes,
                 const AssetHandle<MaterialInstance>& material, const char* name,
                 const vec3& center, f32 edge, Context& context)
    {
        const Ref<Mesh> mesh = Mesh::BuildSync(context, Primitives::Cube(edge, material), name);
        meshes.push_back(mesh);
        const Entity entity = scene.CreateEntity();
        scene.Add<Transform>(entity).Position = center;
        scene.Add<MeshRenderer>(entity).Mesh = assets.Adopt(mesh);
    }

    // A capturing entity at the origin (no mesh) carrying a distance-publishing CaptureSurface, so the
    // distance map is read back directly rather than through a consuming surface.
    Entity AddDistanceCapture(Scene& scene, u32 faceResolution, u32 distanceResolution)
    {
        const Entity entity = scene.CreateEntity();
        scene.Add<Transform>(entity);
        auto& capture = scene.Add<CaptureSurface>(entity);
        capture.Resolution = faceResolution;
        capture.Refresh = CaptureRefresh::EveryFrame;
        // A non-empty DepthTextureSlot is the opt-in that makes the capture publish a distance map.
        capture.DepthTextureSlot = "Depth";
        capture.DepthResolution = distanceResolution;
        return entity;
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
        auto* const built = capture.Drive(Context, assets, *scene, surfaceEntity, vec3(0.0f), 0.0f,
                                          mat3(1.0f), material);
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
                  "capture surface: an entity-aligned basis orients the faces, so the map rotates "
                  "with the carrier")
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

    // The probe material samples the map in the -Y direction, where the green backdrop sits, so a
    // world-aligned capture renders green there (the first case). A basis rotating the carrier 180
    // degrees about X aims the -Y face at world +Y instead — empty scene — so the same -Y sample now
    // reads black. That the captured content moves with the basis is the whole of entity-alignment:
    // the faces are oriented in the carrier's frame, and the map is sampled in it.
    const mat3 flipX(vec3(1.0f, 0.0f, 0.0f), vec3(0.0f, -1.0f, 0.0f), vec3(0.0f, 0.0f, -1.0f));

    vector<u8> output;
    for (u32 frame = 0; frame < SceneCapture::FaceCount; ++frame)
    {
        auto* const built = capture.Drive(Context, assets, *scene, surfaceEntity, vec3(0.0f), 0.0f,
                                          flipX, material);
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

    // The -Y sample now looks at empty world +Y, so the surface is no longer green-dominant — where a
    // world-aligned capture (mat3(1.0f)) rendered it green from the same scene and sample direction.
    const vec4 center = SampleBlock(output, Extent, vec2(0.5f, 0.5f));
    CHECK(center.g < 0.3f);
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
        auto* const built = capture.Drive(Context, assets, *scene, surfaceEntity, vec3(0.0f), 0.0f,
                                          mat3(1.0f), material);
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
            capture.Drive(Context, assets, *scene, surfaceEntity, vec3(0.0f), 0.0f, mat3(1.0f),
                          material);
        }

        // Its FaceCount-face refresh complete, an on-demand capture idles — it pushes no more views.
        CHECK_FALSE(capture.IsRefreshing());
        capture.Drive(Context, assets, *scene, surfaceEntity, vec3(0.0f), 0.0f, mat3(1.0f),
                      material);
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
            capture.Drive(Context, assets, *scene, surfaceEntity, vec3(0.0f), 0.0f, mat3(1.0f),
                          material);
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

    const vec4 up = RenderFrame(capture.Drive(Context, assets, *scene, surfaceEntity,
                                              vec3(0.0f, 4.0f, 0.0f), 0.0f, mat3(1.0f), material));
    CHECK(up.g > 0.6f);
    CHECK(up.g > up.r + 0.3f);
    CHECK(up.g > up.b + 0.3f);

    const vec4 right =
        RenderFrame(capture.Drive(Context, assets, *scene, surfaceEntity, vec3(4.0f, 0.0f, 0.0f),
                                  0.0f, mat3(1.0f), material));
    CHECK(right.r > 0.6f);
    CHECK(right.r > right.g + 0.3f);
    CHECK(right.r > right.b + 0.3f);

    const vec4 forward =
        RenderFrame(capture.Drive(Context, assets, *scene, surfaceEntity, vec3(0.0f, 0.0f, 4.0f),
                                  0.0f, mat3(1.0f), material));
    CHECK(forward.b > 0.6f);
    CHECK(forward.b > forward.r + 0.3f);
    CHECK(forward.b > forward.g + 0.3f);

    // An empty CenterSlot publishes nothing, so the last centre stands: the slot is opt-in, and a
    // material that only samples by direction declares no such field for the drive to find.
    capture.CenterSlot.clear();
    const vec4 unpublished =
        RenderFrame(capture.Drive(Context, assets, *scene, surfaceEntity, vec3(0.0f, 4.0f, 0.0f),
                                  0.0f, mat3(1.0f), material));
    CHECK(unpublished.b > 0.6f);
    CHECK(unpublished.b > unpublished.g + 0.3f);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "capture surface: the drive publishes the frame the faces were oriented in")
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
    capture.OrientationSlot = "Orientation";
    const AssetHandle<MaterialInstance> material = SurfaceMaterial(*scene, surfaceEntity);
    const Unique<Viewport> viewport = MakeViewport(Context, assets);

    // The centre slot stays unnamed and its validity flag is set by hand, so the fragment's fallback
    // is not what this case reads — and the unbind below then touches the frame slot alone, which is
    // what makes the identity it writes there observable.
    material->SetParam("Center", vec4(0.0f, 0.0f, 0.0f, 1.0f));

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

    const auto DriveWith = [&](const mat3& faceBasis)
    {
        return capture.Drive(Context, assets, *scene, surfaceEntity, vec3(0.0f), 0.0f, faceBasis,
                             material);
    };
    const auto Quarter = [](const vec3& axis)
    { return mat3(glm::rotate(mat4(1.0f), glm::radians(90.0f), axis)); };

    // The identity basis a World-aligned capture is driven with publishes the identity rotation: an
    // imaginary part of zero and a real part of one. That is what lets a consumer read the slot with
    // no branch on the alignment — world space is a frame like any other.
    material->SetParam("Display", vec4(0.0f, 1.0f, 0.0f, 0.0f));
    const vec4 worldXyz = RenderFrame(DriveWith(mat3(1.0f)));
    CHECK(worldXyz.r < 0.15f);
    CHECK(worldXyz.g < 0.15f);
    CHECK(worldXyz.b < 0.15f);

    material->SetParam("Display", vec4(0.0f, 0.0f, 1.0f, 0.0f));
    const vec4 worldW = RenderFrame(DriveWith(mat3(1.0f)));
    CHECK(worldW.r > 0.5f);
    CHECK(std::abs(worldW.r - worldW.g) < 0.05f);
    CHECK(std::abs(worldW.r - worldW.b) < 0.05f);

    // A carrier's own rotation publishes as that rotation: a quarter turn about an axis puts the
    // quaternion's imaginary part on that axis, so the channel that lights up names the axis. Two
    // axes in turn, so the value is shown to be the basis handed to Drive rather than a constant.
    material->SetParam("Display", vec4(0.0f, 1.0f, 0.0f, 0.0f));

    const vec4 aboutY = RenderFrame(DriveWith(Quarter(vec3(0.0f, 1.0f, 0.0f))));
    CHECK(aboutY.g > 0.4f);
    CHECK(aboutY.g > aboutY.r + 0.3f);
    CHECK(aboutY.g > aboutY.b + 0.3f);

    const vec4 aboutX = RenderFrame(DriveWith(Quarter(vec3(1.0f, 0.0f, 0.0f))));
    CHECK(aboutX.r > 0.4f);
    CHECK(aboutX.r > aboutX.g + 0.3f);
    CHECK(aboutX.r > aboutX.b + 0.3f);

    // Teardown returns the frame to the identity rather than to a zero vec4: the centre's flag is
    // what gates a consumer's sample, so an unbound frame is unread — but a zero quaternion
    // normalizes to a NaN in one that reads it ungated, and the identity is world space.
    capture.Unbind();
    const vec4 unboundXyz = RenderFrame(nullptr);
    CHECK(unboundXyz.r < 0.15f);
    CHECK(unboundXyz.g < 0.15f);
    CHECK(unboundXyz.b < 0.15f);

    material->SetParam("Display", vec4(0.0f, 0.0f, 1.0f, 0.0f));
    const vec4 unboundW = RenderFrame(nullptr);
    CHECK(unboundW.r > 0.5f);
    CHECK(std::abs(unboundW.r - unboundW.g) < 0.05f);
    CHECK(std::abs(unboundW.r - unboundW.b) < 0.05f);
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
            auto* const built = capture.Drive(Context, assets, *scene, surfaceEntity, vec3(0.0f),
                                              0.0f, mat3(1.0f), material);
            REQUIRE(built != nullptr);
            captured = RenderFrame(built);
        }
        CHECK(captured.g > 0.6f);
        CHECK(captured.g > captured.r + 0.3f);
        CHECK(captured.g > captured.b + 0.3f);

        if (removeComponent)
        {
            (void)scene->Remove<CaptureSurface>(surfaceEntity);
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
        auto* const built = capture.Drive(Context, assets, scene, entity, vec3(0.0f), 0.0f,
                                          mat3(1.0f), SurfaceMaterial(scene, entity));
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

        (void)scene->Remove<CaptureSurface>(surfaceEntity);
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

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "capture surface: a built and dropped surface returns every bindless slot it took")
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

    const BindlessRegistry& registry = Context.GetBindlessRegistry();

    // A release is deferred until AcquireNextFrame has cycled past every frame-in-flight that could
    // still be sampling the slot, so both readings are taken after a full cycle — otherwise the
    // counts would report the deferral rather than the ownership.
    const auto SettleReleases = [&]
    {
        for (u32 frame = 0; frame < Context.GetMaxFramesInFlight() + 1; ++frame)
        {
            Context.BeginFrame();
            Context.EndFrame();
        }
    };

    // One build-and-drop of a scene carrying a capture surface, driven once — the drive is what
    // materializes the capture and the sampler the material reads its output through, so a surface
    // that never drove holds nothing to hand back.
    const auto BuildDriveAndDrop = [&]
    {
        vector<Ref<Mesh>> meshes;
        Entity surfaceEntity;
        const Unique<Scene> scene =
            BuildCaptureScene(Context, assets, Types, *probe, *backdrop, CaptureRefresh::EveryFrame,
                              meshes, surfaceEntity);
        auto& capture = scene->Get<CaptureSurface>(surfaceEntity);
        capture.Resolution = 32;
        REQUIRE(capture.Drive(Context, assets, *scene, surfaceEntity, vec3(0.0f), 0.0f, mat3(1.0f),
                              SurfaceMaterial(*scene, surfaceEntity)) != nullptr);
    };

    // The baseline is taken after one cycle, not before it: the first capture built in a run loads
    // the shared engine assets its renderer needs, and those stay resident in the asset manager for
    // the rest of it. What a leak shows up in is the steady-state cost of a cycle, so that is what
    // the readings bracket.
    BuildDriveAndDrop();
    SettleReleases();
    const BindlessCapacity before = registry.GetFreeSlots();
    REQUIRE(before.Samplers > 0);

    // The count is the invariant, not any particular figure: a cycle that hands back one slot fewer
    // than it took exhausts its array after as many cycles as the array holds slots, and exhaustion
    // is fatal. Several cycles, so a delta is read as a per-cycle rate rather than a one-off.
    constexpr u32 Cycles = 8;
    for (u32 cycle = 0; cycle < Cycles; ++cycle)
    {
        BuildDriveAndDrop();
        SettleReleases();

        const BindlessCapacity after = registry.GetFreeSlots();
        CHECK(after.Samplers == before.Samplers);
        CHECK(after.Textures == before.Textures);
        CHECK(after.StorageImages == before.StorageImages);
        CHECK(after.StorageBuffers == before.StorageBuffers);
        CHECK(after.Materials == before.Materials);
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
            auto* const built = capture.Drive(Context, assets, *scene, shellEntity, probe, 0.0f,
                                              mat3(1.0f), material);
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
            auto* const built = capture.Drive(Context, assets, *scene, shellEntity, vec3(0.0f),
                                              0.0f, mat3(1.0f), material);
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

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "capture surface: the world drive centres the probe on the drawn pose")
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
    Unique<Scene> built = BuildCaptureScene(Context, assets, Types, *parallax, *backdrop,
                                            CaptureRefresh::EveryFrame, meshes, surfaceEntity);

    // Mount the capture-bearing entity on a carrier that moved a whole tick, so the pose it is drawn
    // at depends on the alpha. The travel is along the camera's own axis, which keeps the surface
    // centred in frame at every alpha — the sample point has to stay on the surface for the frame to
    // read its material at all. The constant y keeps the published centre's green non-zero
    // throughout, so a frame that read a centre is distinguishable from one that read nothing.
    const Entity carrier = built->CreateEntity();
    built->Add<Transform>(carrier).Position = vec3(0.0f, 0.5f, 0.0f);
    built->SetParent(surfaceEntity, carrier);

    built->SnapshotTransformHistory();
    built->Get<Transform>(carrier).Position = vec3(0.0f, 0.5f, 1.0f);
    built->SnapshotTransformHistory();
    REQUIRE(built->HasTransformInterpolation());

    auto& capture = built->Get<CaptureSurface>(surfaceEntity);
    capture.CenterSlot = "Center";
    const AssetHandle<MaterialInstance> material = SurfaceMaterial(*built, surfaceEntity);
    // Display the published centre as radiance rather than the captured colour.
    material.Get()->SetParam("Display", vec4(1.0f, 0.0f, 0.0f, 0.0f));

    // The drive reads its alpha off the world it is walking, so hand the scene to a runner and set
    // that world's alpha — the same field the frame's tick leaves behind.
    SystemRegistry systems;
    WorldRunner runner({
        .Types = &Types,
        .Systems = &systems,
        .Assets = &assets,
        .Context = &Context,
    });
    // Run state does not gate capture driving, so the world needs no started simulation (which would
    // in turn need a start context this case has no use for) — only presentation does.
    const WorldInstanceId world = runner.OpenWorld(WorldOpenInfo{.StartSimulation = false});
    Scene& scene = runner.InstallScene(world, std::move(built));
    const Unique<Viewport> viewport = MakeViewport(Context, assets);

    // Drives every capture surface in the runner's worlds at the world's alpha, then renders one
    // frame; the surface fills the frame centre, so the sample *is* the centre the drive published.
    const auto DriveAndRender = [&](const f32 alpha)
    {
        runner.ResolveWorld(world)->LastAlpha = alpha;
        SceneCapture* driven = nullptr;
        runner.DriveCaptureSurfaces({
            .Register = [&](SceneCapture& c) { driven = &c; },
            .IsPresented = [](WorldInstanceId) { return true; },
        });
        if (driven == nullptr)
        {
            driven = scene.Get<CaptureSurface>(surfaceEntity).GetCapture();
        }
        viewport->SetViewState({.World = &scene, .Camera = FrontCamera(), .Delta = 0.016f});
        Context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                driven->Render(cmd);
                viewport->Render(cmd);
            });
        const vector<u8> output = viewport->GetOutput()->GetImage()->Download();
        return SampleBlock(output, Extent, vec2(0.5f, 0.5f));
    };

    // Alpha 1 is the pose the carrier ended the tick at, which is what the un-interpolated world
    // matrix also reads — so the published centre carries the carrier's whole tick of travel.
    const vec4 atOne = DriveAndRender(1.0f);
    CHECK(atOne.g > 0.1f);
    CHECK(atOne.b > 0.1f);

    // Alpha 0 is the pose it started the tick at: the same entity, the same frame, a centre with no
    // travel in it at all. That the two differ is the property the drive exists to provide — a probe
    // pinned to the un-interpolated pose would publish the alpha-1 centre on every frame regardless,
    // which is what makes it disagree with the mesh it feeds by a fraction of a tick's motion.
    const vec4 atZero = DriveAndRender(0.0f);
    CHECK(atZero.g > 0.1f);
    CHECK(atZero.b < 0.05f);
    CHECK(atOne.b > atZero.b + 0.1f);

    // Midway lies between the two, so the centre tracks the alpha continuously rather than snapping
    // between snapshots — which is why a wrong alpha reads as a per-frame wobble and not as a jump.
    const vec4 atHalf = DriveAndRender(0.5f);
    CHECK(atHalf.b > atZero.b + 0.02f);
    CHECK(atHalf.b < atOne.b - 0.02f);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "capture surface: only a presented world's captures are driven, and a world that "
                  "goes dark is re-armed")
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

    // Two live worlds, each carrying one on-demand capture surface — the ordinary state of a runner
    // holding worlds warm while one of them is on screen.
    SystemRegistry systems;
    WorldRunner runner({
        .Types = &Types,
        .Systems = &systems,
        .Assets = &assets,
        .Context = &Context,
    });

    vector<Ref<Mesh>> meshes;
    Entity firstEntity;
    Entity secondEntity;
    Unique<Scene> firstScene = BuildCaptureScene(Context, assets, Types, *probe, *backdrop,
                                                 CaptureRefresh::OnDemand, meshes, firstEntity);
    Unique<Scene> secondScene = BuildCaptureScene(Context, assets, Types, *probe, *backdrop,
                                                  CaptureRefresh::OnDemand, meshes, secondEntity);

    const WorldInstanceId first = runner.OpenWorld(WorldOpenInfo{.StartSimulation = false});
    runner.InstallScene(first, std::move(firstScene));
    const WorldInstanceId second = runner.OpenWorld(WorldOpenInfo{.StartSimulation = false});
    runner.InstallScene(second, std::move(secondScene));

    const auto SurfaceOf = [&](const WorldInstanceId world, const Entity entity) -> const auto&
    { return runner.ResolveWorld(world)->GetScene().Get<CaptureSurface>(entity); };

    WorldInstanceId presented = first;
    u32 registered = 0;
    const auto Drive = [&]
    {
        return runner.DriveCaptureSurfaces({
            .Register = [&registered](SceneCapture&) { ++registered; },
            .IsPresented = [&presented](const WorldInstanceId world) { return world == presented; },
        });
    };

    // Only the presented world is walked: its surface materializes and registers a capture, while the
    // world nothing shows is skipped whole — no capture built, so no face render and no view slot.
    const WorldCaptureDriveResult firstPass = Drive();
    CHECK(firstPass.WorldsDriven == 1);
    CHECK(firstPass.WorldsSkipped == 1);
    CHECK(firstPass.SurfacesDriven == 1);
    CHECK(registered == 1);
    CHECK(SurfaceOf(first, firstEntity).GetCapture() != nullptr);
    CHECK(SurfaceOf(second, secondEntity).GetCapture() == nullptr);
    // The unpresented capture is not quietly settled either: it still owes its first refresh, so
    // presenting that world renders a map rather than leaving one blank.
    CHECK(SurfaceOf(second, secondEntity).IsRefreshing());

    // Settle the presented world's on-demand refresh: one face per driven frame.
    for (u32 face = 1; face < SceneCapture::FaceCount; ++face)
    {
        Drive();
    }
    CHECK_FALSE(SurfaceOf(first, firstEntity).IsRefreshing());

    // Presentation moves to the other world. The world that went dark drives nothing, and its settled
    // capture is re-armed — so when it is presented again it rebuilds its map instead of resuming from
    // the scene as it stood before it went out of view.
    presented = second;
    const WorldCaptureDriveResult secondPass = Drive();
    CHECK(secondPass.WorldsDriven == 1);
    CHECK(secondPass.WorldsSkipped == 1);
    CHECK(secondPass.SurfacesDriven == 1);
    CHECK(secondPass.SurfacesReArmed == 1);
    CHECK(SurfaceOf(first, firstEntity).IsRefreshing());
    CHECK(SurfaceOf(second, secondEntity).GetCapture() != nullptr);
    CHECK(registered == 2);
}

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "capture surface: a spent view budget drops captures instead of aborting, and the "
    "presented viewport keeps its slot")
{
    RegisterBuiltinTypes(Types);

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(CookCapturePack()).has_value());

    const AssetResult<AssetHandle<MaterialInstance>> backdrop =
        assets.LoadSync<MaterialInstance>(BackdropInstance);
    REQUIRE(backdrop.has_value());

    // A scene whose one bright cube fills the frame, so the viewport's own render is legible in its
    // output: it rendered iff the centre is bright.
    const Unique<Scene> scene = Scene::Create(Types);
    const Ref<Mesh> cube =
        Mesh::BuildSync(Context, Primitives::Cube(2.0f, *backdrop), "Budget Cube");
    const Entity cubeEntity = scene->CreateEntity();
    scene->Add<Transform>(cubeEntity);
    scene->Add<MeshRenderer>(cubeEntity).Mesh = assets.Adopt(cube);

    ViewportCompositor compositor(Context);
    const Unique<Viewport> viewport = MakeViewport(Context, assets);
    viewport->SetViewState({.World = scene.get(), .Camera = FrontCamera(), .Delta = 0.016f});
    compositor.RegisterViewport(*viewport);

    // More captures than the frame can afford. Each claims one view slot per driven frame, so the
    // budget is narrowed by hand instead of by registering thirty-odd renderers: the drive's decision
    // reads the same either way.
    constexpr u32 CaptureCount = 6;
    constexpr u32 SlotsLeftForFrame = 4;
    vector<Unique<SceneCapture>> captures;
    for (u32 i = 0; i < CaptureCount; ++i)
    {
        captures.push_back(SceneCapture::Create({
            .Context = Context,
            .Assets = assets,
            .FaceResolution = 32,
        }));
        compositor.RegisterCapture(*captures.back());
    }

    BindlessRegistry& registry = Context.GetBindlessRegistry();
    registry.OnFrameAcquired(0);
    while (registry.GetRemainingViews() > SlotsLeftForFrame)
    {
        CHECK(registry.TryBeginView());
    }
    for (const Unique<SceneCapture>& capture : captures)
    {
        capture->SetView({.World = scene.get()});
    }

    // The frame records without aborting: three captures fit beside the viewport's reserved slot, the
    // rest hold their last map.
    Context.ImmediateCommands([&](CommandBuffer& cmd) { compositor.RenderRegistered(cmd); });

    // Every slot is spent and none was overdrawn — the whole budget was claimed, one of it the
    // viewport's, and the claim past the end is refused rather than fatal.
    CHECK(registry.GetRemainingViews() == 0);
    CHECK_FALSE(registry.TryBeginView());

    // The viewport rendered: the captures gave way to it, which is the priority a stale window would
    // have inverted.
    const vector<u8> output = viewport->GetOutput()->GetImage()->Download();
    const vec4 center = SampleBlock(output, Extent, vec2(0.5f, 0.5f));
    CHECK(center.g > 0.6f);
}

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "capture distance: a capture centred in a box records radial distances, with a sky "
    "sentinel where the box is open")
{
    RegisterBuiltinTypes(Types);

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(CookCapturePack()).has_value());

    const AssetResult<AssetHandle<MaterialInstance>> wall =
        assets.LoadSync<MaterialInstance>(WallGreenInstance);
    REQUIRE(wall.has_value());

    // Five opaque walls forming a box of half-extent H = 4 about the origin, open at +Y. Each wall is
    // a cube of edge 12 (half 6) whose inner face sits at ±4: wide enough that every ray whose
    // dominant axis is the wall's normal lands on that inner face.
    constexpr f32 H = 4.0f;
    vector<Ref<Mesh>> meshes;
    const Unique<Scene> scene = Scene::Create(Types);
    AddWall(*scene, assets, meshes, *wall, "Box +X", vec3(10.0f, 0.0f, 0.0f), 12.0f, Context);
    AddWall(*scene, assets, meshes, *wall, "Box -X", vec3(-10.0f, 0.0f, 0.0f), 12.0f, Context);
    AddWall(*scene, assets, meshes, *wall, "Box -Y", vec3(0.0f, -10.0f, 0.0f), 12.0f, Context);
    AddWall(*scene, assets, meshes, *wall, "Box +Z", vec3(0.0f, 0.0f, 10.0f), 12.0f, Context);
    AddWall(*scene, assets, meshes, *wall, "Box -Z", vec3(0.0f, 0.0f, -10.0f), 12.0f, Context);

    const Entity captureEntity = AddDistanceCapture(*scene, 128, 128);
    const CaptureSurface& capture = scene->Get<CaptureSurface>(captureEntity);

    // Drive a full round-robin refresh, then read the distance map back directly.
    for (u32 frame = 0; frame < SceneCapture::FaceCount; ++frame)
    {
        auto* const built = capture.Drive(Context, assets, *scene, captureEntity, vec3(0.0f), 0.0f,
                                          mat3(1.0f), AssetHandle<MaterialInstance>{});
        REQUIRE(built != nullptr);
        Context.ImmediateCommands([&](CommandBuffer& cmd) { built->Render(cmd); });
    }

    const SceneCapture* built = capture.GetCapture();
    REQUIRE(built != nullptr);
    REQUIRE(built->GetDistanceOutput() != nullptr);
    const u32 edge = built->GetDistanceOutput()->GetImage()->GetExtent().x;
    const vector<u8> map = built->GetDistanceOutput()->GetImage()->Download();

    // Face centres (slightly off-axis, so they map to interior texels rather than the octahedral
    // fold's edges): each wall stands at H along its axis, so its radial distance is ~H.
    const vec3 faceDirs[5] = {
        vec3(1.0f, 0.05f, 0.05f), vec3(-1.0f, 0.05f, 0.05f), vec3(0.05f, -1.0f, 0.05f),
        vec3(0.05f, 0.05f, 1.0f), vec3(0.05f, 0.05f, -1.0f),
    };
    for (const vec3& dir : faceDirs)
    {
        const f32 expected = BoxRadialDistance(H, dir);
        const f32 recorded = DistanceInDirection(map, edge, dir);
        CHECK(recorded == doctest::Approx(expected).epsilon(0.06));
        // A direction with geometry never reads the sky sentinel.
        CHECK(recorded < SceneCapture::DistanceSkySentinel * 0.5f);
    }

    // The single most valuable check: an off-axis texel is RADIAL, not the face-axis distance. For a
    // direction 42 degrees off the +X axis the radial distance to the +X wall is H/cos, which the
    // z-along-the-axis answer (H) is not — so the recorded value must match the radial length and sit
    // clearly above H.
    const vec3 offAxis(1.0f, -0.9f, 0.1f);
    const f32 radial = BoxRadialDistance(H, offAxis); // ~5.39, versus the face-axis answer H = 4.
    const f32 recordedOffAxis = DistanceInDirection(map, edge, offAxis);
    CHECK(recordedOffAxis == doctest::Approx(radial).epsilon(0.08));
    CHECK(recordedOffAxis > H + 0.6f);

    // The box is open at +Y: that direction saw no geometry, so it records the sky sentinel.
    const f32 openSky = DistanceInDirection(map, edge, vec3(0.05f, 1.0f, 0.05f));
    CHECK(openSky >= SceneCapture::DistanceSkySentinel);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "capture distance: the distance map registers with the radiance map")
{
    RegisterBuiltinTypes(Types);

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(CookCapturePack()).has_value());

    const AssetResult<AssetHandle<MaterialInstance>> shell =
        assets.LoadSync<MaterialInstance>(ShellInstance);
    const AssetResult<AssetHandle<MaterialInstance>> green =
        assets.LoadSync<MaterialInstance>(WallGreenInstance);
    const AssetResult<AssetHandle<MaterialInstance>> red =
        assets.LoadSync<MaterialInstance>(WallRedInstance);
    REQUIRE(shell.has_value());
    REQUIRE(green.has_value());
    REQUIRE(red.has_value());

    // Two distinguishable walls in known directions and at distinct distances: green straight down
    // (its inner face at Y = -4, so distance 4) and red straight ahead (its inner face at Z = 10, so
    // distance 10). A shell at the origin carries the capture and samples the RADIANCE map in an
    // authored direction; the DISTANCE map is read back directly.
    vector<Ref<Mesh>> meshes;
    const Unique<Scene> scene = Scene::Create(Types);
    AddWall(*scene, assets, meshes, *green, "Reg Green Down", vec3(0.0f, -10.0f, 0.0f), 12.0f,
            Context);
    AddWall(*scene, assets, meshes, *red, "Reg Red Ahead", vec3(0.0f, 0.0f, 16.0f), 12.0f, Context);

    const Ref<Mesh> shellMesh =
        Mesh::BuildSync(Context, Primitives::Cube(1.4f, *shell), "Reg Shell");
    meshes.push_back(shellMesh);
    const Entity shellEntity = scene->CreateEntity();
    scene->Add<Transform>(shellEntity);
    scene->Add<MeshRenderer>(shellEntity).Mesh = assets.Adopt(shellMesh);
    auto& capture = scene->Add<CaptureSurface>(shellEntity);
    capture.Resolution = 128;
    capture.Refresh = CaptureRefresh::EveryFrame;
    capture.DepthTextureSlot = "Depth";
    capture.DepthResolution = 128;

    const AssetHandle<MaterialInstance> material = SurfaceMaterial(*scene, shellEntity);
    const Unique<Viewport> viewport = MakeViewport(Context, assets);

    // Drive a full refresh with the shell showing the -Y radiance, and read both maps once populated.
    material->SetParam("Direction", vec4(0.0f, -1.0f, 0.0f, 0.0f));
    vector<u8> frame;
    for (u32 i = 0; i < SceneCapture::FaceCount; ++i)
    {
        auto* const built = capture.Drive(Context, assets, *scene, shellEntity, vec3(0.0f), 0.0f,
                                          mat3(1.0f), material);
        REQUIRE(built != nullptr);
        viewport->SetViewState({.World = scene.get(), .Camera = FrontCamera(), .Delta = 0.016f});
        Context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                built->Render(cmd);
                viewport->Render(cmd);
            });
        frame = viewport->GetOutput()->GetImage()->Download();
    }

    // Radiance at -Y is the green wall.
    const vec4 downColor = SampleBlock(frame, Extent, vec2(0.5f, 0.5f));
    CHECK(downColor.g > 0.5f);
    CHECK(downColor.g > downColor.r + 0.2f);

    // Distance at -Y is the green wall's distance (4), and at +Z the red wall's (10). The same
    // OctahedralUV(direction) addresses the same surface in both maps: green sits at distance 4 in the
    // exact direction its colour appears, red at 10 where its colour appears — a misregistered
    // distance map would not line the two up.
    const SceneCapture* built = capture.GetCapture();
    REQUIRE(built->GetDistanceOutput() != nullptr);
    const u32 edge = built->GetDistanceOutput()->GetImage()->GetExtent().x;
    const vector<u8> map = built->GetDistanceOutput()->GetImage()->Download();
    CHECK(DistanceInDirection(map, edge, vec3(0.05f, -1.0f, 0.05f)) ==
          doctest::Approx(4.0f).epsilon(0.08));
    CHECK(DistanceInDirection(map, edge, vec3(0.05f, 0.05f, 1.0f)) ==
          doctest::Approx(10.0f).epsilon(0.08));

    // Radiance at +Z is the red wall — the colour half of the same registration.
    material->SetParam("Direction", vec4(0.0f, 0.0f, 1.0f, 0.0f));
    for (u32 i = 0; i < SceneCapture::FaceCount; ++i)
    {
        auto* const built2 = capture.Drive(Context, assets, *scene, shellEntity, vec3(0.0f), 0.0f,
                                           mat3(1.0f), material);
        viewport->SetViewState({.World = scene.get(), .Camera = FrontCamera(), .Delta = 0.016f});
        Context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                built2->Render(cmd);
                viewport->Render(cmd);
            });
        frame = viewport->GetOutput()->GetImage()->Download();
    }
    const vec4 aheadColor = SampleBlock(frame, Extent, vec2(0.5f, 0.5f));
    CHECK(aheadColor.r > 0.5f);
    CHECK(aheadColor.r > aheadColor.g + 0.2f);
}

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "capture distance: an unset DepthTextureSlot publishes no distance map; naming one "
    "publishes and binds it")
{
    RegisterBuiltinTypes(Types);

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(CookCapturePack()).has_value());

    const AssetResult<AssetHandle<MaterialInstance>> probe =
        assets.LoadSync<MaterialInstance>(ProbeInstance);
    const AssetResult<AssetHandle<MaterialInstance>> distanceProbe =
        assets.LoadSync<MaterialInstance>(DistanceProbeInstance);
    const AssetResult<AssetHandle<MaterialInstance>> wall =
        assets.LoadSync<MaterialInstance>(WallGreenInstance);
    REQUIRE(probe.has_value());
    REQUIRE(distanceProbe.has_value());
    REQUIRE(wall.has_value());

    SUBCASE("an unset DepthTextureSlot allocates and binds no distance map")
    {
        vector<Ref<Mesh>> meshes;
        Entity surfaceEntity;
        // The default DepthTextureSlot is empty, so this capture opts into no distance map.
        const Unique<Scene> scene =
            BuildCaptureScene(Context, assets, Types, *probe, *probe, CaptureRefresh::EveryFrame,
                              meshes, surfaceEntity);
        const CaptureSurface& capture = scene->Get<CaptureSurface>(surfaceEntity);
        REQUIRE(capture.DepthTextureSlot.empty());

        auto* const built = capture.Drive(Context, assets, *scene, surfaceEntity, vec3(0.0f), 0.0f,
                                          mat3(1.0f), SurfaceMaterial(*scene, surfaceEntity));
        REQUIRE(built != nullptr);
        // The capture reports no distance resources: no distance map view, an invalid handle.
        CHECK(built->GetDistanceOutput() == nullptr);
        CHECK_FALSE(built->GetDistanceOutputHandle().IsValid());
    }

    SUBCASE("naming a DepthTextureSlot publishes the map and binds it end to end")
    {
        // A green floor 8 units below (inner face at Y = -4, distance 4), and a distance-probe surface
        // at the origin that samples the distance map in -Y and shows the recorded distance.
        vector<Ref<Mesh>> meshes;
        const Unique<Scene> scene = Scene::Create(Types);
        AddWall(*scene, assets, meshes, *wall, "Opt Floor", vec3(0.0f, -10.0f, 0.0f), 12.0f,
                Context);

        const Ref<Mesh> surface =
            Mesh::BuildSync(Context, Primitives::Cube(1.4f, *distanceProbe), "Opt Probe");
        meshes.push_back(surface);
        const Entity surfaceEntity = scene->CreateEntity();
        scene->Add<Transform>(surfaceEntity);
        scene->Add<MeshRenderer>(surfaceEntity).Mesh = assets.Adopt(surface);
        auto& capture = scene->Add<CaptureSurface>(surfaceEntity);
        capture.Resolution = 128;
        capture.Refresh = CaptureRefresh::EveryFrame;
        capture.DepthTextureSlot = "Depth";
        capture.DepthSamplerSlot = "DepthSampler";
        capture.DepthResolution = 128;

        const AssetHandle<MaterialInstance> material = SurfaceMaterial(*scene, surfaceEntity);
        material->SetParam("Direction", vec4(0.0f, -1.0f, 0.0f, 0.0f));
        const Unique<Viewport> viewport = MakeViewport(Context, assets);

        vector<u8> frame;
        for (u32 i = 0; i < SceneCapture::FaceCount; ++i)
        {
            auto* const built = capture.Drive(Context, assets, *scene, surfaceEntity, vec3(0.0f),
                                              0.0f, mat3(1.0f), material);
            REQUIRE(built != nullptr);
            viewport->SetViewState(
                {.World = scene.get(), .Camera = FrontCamera(), .Delta = 0.016f});
            Context.ImmediateCommands(
                [&](CommandBuffer& cmd)
                {
                    built->Render(cmd);
                    viewport->Render(cmd);
                });
            frame = viewport->GetOutput()->GetImage()->Download();
        }

        // The capture now reports a distance map, and its handle is valid.
        const SceneCapture* built = capture.GetCapture();
        CHECK(built->GetDistanceOutput() != nullptr);
        CHECK(built->GetDistanceOutputHandle().IsValid());

        // End to end: the probe sampled the distance map through the bound Depth/DepthSampler slots
        // and showed the floor's distance (4) as radiance — a grey, bright centre. Had the slots not
        // bound, it would sample the cooked-default brick texture (a coloured, non-grey value).
        const vec4 center = SampleBlock(frame, Extent, vec2(0.5f, 0.5f));
        CHECK(center.r > 0.3f);
        CHECK(std::abs(center.r - center.g) < 0.1f);
        CHECK(std::abs(center.r - center.b) < 0.1f);
    }
}
