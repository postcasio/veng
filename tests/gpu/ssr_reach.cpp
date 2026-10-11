// Screen-space reflections reach as far as the view authors, without inventing hits. Four cases:
//
//  - a mirror floor under a red wall 800 m away reflects the wall's colour once SsrMaxDistance
//    reaches it, at the default thickness, where a short ray reflects nothing;
//  - a thin board floating close to the camera is not reflected into the floor seen through the
//    gap beneath it — those rays pass far behind the board — while its true mirror image below
//    the gap is;
//  - the step budget, distance and thickness authored on a scene's RenderLook reach the trace when
//    a viewport presents the scene;
//  - a smooth mirror seen from just above it reflects an emitter 300 m away as fully as one 30 m
//    away: a reflection does not fade with how far away its hit lies. This case runs the
//    stochastic chain and reads its raw intersection, which resolves the end of each ray's
//    screen line in depth.
//
// The materials are forward_opaque's untextured parameter block, so a white metal floor reflects
// exactly the colour it is shown. Skips cleanly (exit 77) on a machine with no Vulkan ICD.

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Primitives.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/LightPacking.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Renderer/ViewportCompositor.h>
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

    constexpr uvec2 Extent{128, 128};

    vec3 DecodeTexel(const vector<u8>& rgba16f, u32 width, u32 x, u32 y)
    {
        const auto* halves = reinterpret_cast<const u16*>(rgba16f.data());
        const usize base = (static_cast<usize>(y) * width + x) * 4;
        return vec3(glm::unpackHalf1x16(halves[base + 0]), glm::unpackHalf1x16(halves[base + 1]),
                    glm::unpackHalf1x16(halves[base + 2]));
    }

    // The mean colour over rows [y0, y1) and columns [x0, x1).
    vec3 MeanColour(const vector<u8>& pixels, u32 x0, u32 x1, u32 y0, u32 y1)
    {
        vec3 sum(0.0f);
        for (u32 y = y0; y < y1; ++y)
        {
            for (u32 x = x0; x < x1; ++x)
            {
                sum += DecodeTexel(pixels, Extent.x, x, y);
            }
        }
        return sum / static_cast<f32>((x1 - x0) * (y1 - y0));
    }

    f32 Luma(const vec3& c)
    {
        return glm::dot(c, vec3(0.2126f, 0.7152f, 0.0722f));
    }

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
                                               const char* name, const vec3& colour, f32 roughness,
                                               f32 metallic)
    {
        const AssetResult<AssetHandle<Material>> parent = assets.LoadSync<Material>(OpaqueMaterial);
        REQUIRE(parent.has_value());
        const AssetHandle<MaterialInstance> instance = assets.BuildSync<MaterialInstance>(
            MaterialInstanceInfo{.Name = name, .Context = &context, .Parent = *parent});
        REQUIRE(instance.IsLoaded());
        auto& mat = const_cast<MaterialInstance&>(*instance.Get());
        mat.SetParam("BaseColor", vec4(colour, 1.0f));
        mat.SetParam("Roughness", roughness);
        mat.SetParam("Metallic", metallic);
        return instance;
    }

    // A plane standing upright, facing +Z, centred at `centre`.
    Entity AddWall(Scene& scene, const AssetManager& assets, const Ref<Mesh>& plane,
                   const vec3& centre)
    {
        const Entity wall = scene.CreateEntity();
        auto& transform = scene.Add<Transform>(wall);
        transform.Position = centre;
        transform.Rotation = glm::angleAxis(glm::radians(90.0f), vec3(1.0f, 0.0f, 0.0f));
        scene.Add<MeshRenderer>(wall).Mesh = assets.Adopt(plane);
        return wall;
    }

    // A white mirror metal floor at y = 0, a red rough wall 800 m down -Z standing on it, and a
    // light on the wall's face. The camera stands 2 m up looking level down -Z, so the upper half of
    // the frame is the wall and the lower half its reflection.
    struct FarWallScene
    {
        Unique<Scene> World;
        vector<Ref<Mesh>> Meshes;
        CameraView Camera;
    };

    FarWallScene BuildFarWallScene(Context& context, AssetManager& assets, TypeRegistry& types)
    {
        FarWallScene built{.World = Scene::Create(types)};
        Scene& scene = *built.World;

        const Ref<Mesh> floor = Mesh::BuildSync(
            context,
            Primitives::Plane(vec2(4000.0f), uvec2(1),
                              MakeMaterial(context, assets, "Mirror", vec3(1.0f), 0.02f, 1.0f)),
            "Mirror Floor");
        const Ref<Mesh> wall =
            Mesh::BuildSync(context,
                            Primitives::Plane(vec2(2000.0f, 1200.0f), uvec2(1),
                                              MakeMaterial(context, assets, "Far Wall",
                                                           vec3(0.9f, 0.08f, 0.04f), 1.0f, 0.0f)),
                            "Far Wall");
        built.Meshes = {floor, wall};

        const Entity floorEntity = scene.CreateEntity();
        scene.Add<Transform>(floorEntity);
        scene.Add<MeshRenderer>(floorEntity).Mesh = assets.Adopt(floor);
        AddWall(scene, assets, wall, vec3(0.0f, 600.0f, -800.0f));

        const Entity light = scene.CreateEntity();
        scene.Add<Light>(light) = Light{
            .Type = LightType::Directional,
            .Direction = glm::normalize(vec3(0.0f, -0.3f, -1.0f)),
            .Color = vec3(1.0f),
            .Intensity = 4.0f / LuminousAnchor,
        };

        built.Camera.SetPerspective(glm::radians(50.0f), 1.0f, 0.1f, 5000.0f);
        built.Camera.SetView(vec3(0.0f, 2.0f, 0.0f), vec3(0.0f, 2.0f, -100.0f),
                             vec3(0.0f, 1.0f, 0.0f));
        return built;
    }

    const SceneRendererSettings SsrSettings{
        .Mode = DebugView::Final, .Bloom = false, .Shadows = false, .SSR = true, .AO = false};

    // The reflection of the wall: the floor below the horizon, away from the frame's edges.
    vec3 ReflectionBand(const vector<u8>& pixels)
    {
        return MeanColour(pixels, Extent.x * 3 / 10, Extent.x * 7 / 10, Extent.y * 66 / 100,
                          Extent.y * 84 / 100);
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "ssr reach: a mirror floor reflects a wall 800 m away once the ray reaches it")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const path archive = MountPack(assets, "veng_gpu_ssr_reach_far.vengpack");
    const FarWallScene built = BuildFarWallScene(Context, assets, Types);

    const Unique<SceneRenderer> renderer = SceneRenderer::Create({
        .Context = Context,
        .Assets = assets,
        .OutputFormat = Context.GetOutputFormat(),
        .Extent = Extent,
        .Settings = SsrSettings,
    });

    const auto Render = [&](f32 maxDistance) -> vector<u8>
    {
        Context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                renderer->Execute(cmd, Renderer::SceneView{.World = *built.World,
                                                           .Camera = built.Camera,
                                                           .Delta = 0.0f,
                                                           .SsrMaxDistance = maxDistance});
            });
        return renderer->GetOutput()->GetImage()->Download();
    };

    // The thickness stays at its near-range default: the hit at range is the ray's own depth
    // travel per step being accepted, not an authored slab hundreds of metres deep.
    const vector<u8> shortRay = Render(50.0f);
    const vector<u8> longRay = Render(3000.0f);

    const vec3 wall = MeanColour(longRay, Extent.x * 3 / 10, Extent.x * 7 / 10, Extent.y * 20 / 100,
                                 Extent.y * 40 / 100);
    const vec3 unreached = ReflectionBand(shortRay);
    const vec3 reflected = ReflectionBand(longRay) - unreached;

    // The wall is lit and red on screen.
    REQUIRE(wall.r > 0.1f);
    REQUIRE(wall.r > 4.0f * wall.g);
    // A ray too short to reach the wall reflects nothing there; one that reaches it carries the
    // wall's own colour into the floor.
    CHECK(Luma(reflected) > 0.25f * Luma(wall));
    CHECK(reflected.r > 4.0f * reflected.g);

    std::filesystem::remove(archive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "ssr reach: a thin near board is not reflected into the floor behind it, and is "
                  "into the floor in front of it")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const path archive = MountPack(assets, "veng_gpu_ssr_reach_near.vengpack");

    const Unique<Scene> scene = Scene::Create(Types);
    const Ref<Mesh> floor = Mesh::BuildSync(
        Context,
        Primitives::Plane(vec2(40.0f), uvec2(1),
                          MakeMaterial(Context, assets, "Mirror", vec3(1.0f), 0.02f, 1.0f)),
        "Mirror Floor");
    const Ref<Mesh> board =
        Mesh::BuildSync(Context,
                        Primitives::Plane(vec2(1.2f, 0.3f), uvec2(1),
                                          MakeMaterial(Context, assets, "Board",
                                                       vec3(0.9f, 0.08f, 0.04f), 1.0f, 0.0f)),
                        "Thin Board");

    const Entity floorEntity = scene->CreateEntity();
    scene->Add<Transform>(floorEntity);
    scene->Add<MeshRenderer>(floorEntity).Mesh = assets.Adopt(floor);
    // A zero-thickness board floating 0.45 m to 0.75 m above the floor, 2.5 m from the camera.
    AddWall(*scene, assets, board, vec3(0.0f, 0.6f, -0.5f));

    const Entity light = scene->CreateEntity();
    scene->Add<Light>(light) = Light{
        .Type = LightType::Directional,
        .Direction = glm::normalize(vec3(0.0f, -0.3f, -1.0f)),
        .Color = vec3(1.0f),
        .Intensity = 4.0f / LuminousAnchor,
    };

    CameraView camera;
    camera.SetPerspective(glm::radians(70.0f), 1.0f, 0.1f, 100.0f);
    camera.SetView(vec3(0.0f, 1.0f, 2.0f), vec3(0.0f, 0.0f, -2.0f), vec3(0.0f, 1.0f, 0.0f));

    const Unique<SceneRenderer> renderer = SceneRenderer::Create({
        .Context = Context,
        .Assets = assets,
        .OutputFormat = Context.GetOutputFormat(),
        .Extent = Extent,
        .Settings = SsrSettings,
    });

    // A long ray and a thin assumed thickness: every ray from the floor behind the board passes
    // more than the thickness behind it.
    const auto Render = [&](bool ssr) -> vector<u8>
    {
        SceneRendererSettings settings = SsrSettings;
        settings.SSR = ssr;
        renderer->Configure(settings);
        Context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                renderer->Execute(cmd, Renderer::SceneView{.World = *scene,
                                                           .Camera = camera,
                                                           .Delta = 0.0f,
                                                           .SsrMaxDistance = 100.0f,
                                                           .SsrThickness = 0.05f});
            });
        return renderer->GetOutput()->GetImage()->Download();
    };

    const vector<u8> plain = Render(false);
    const vector<u8> traced = Render(true);

    // Locate the board and its mirror image down the centre column: the board is the red run of
    // the plain render, its mirror image the first run below it that the trace adds red to.
    const u32 column = Extent.x / 2;
    u32 boardTop = Extent.y;
    u32 boardBottom = 0;
    for (u32 y = 0; y < Extent.y; ++y)
    {
        const vec3 c = DecodeTexel(plain, Extent.x, column, y);
        if (c.r > 0.05f && c.r > 3.0f * c.g)
        {
            boardTop = std::min(boardTop, y);
            boardBottom = std::max(boardBottom, y);
        }
    }
    REQUIRE(boardTop < boardBottom);
    u32 imageTop = Extent.y;
    for (u32 y = boardBottom + 1; y < Extent.y; ++y)
    {
        const vec3 added =
            DecodeTexel(traced, Extent.x, column, y) - DecodeTexel(plain, Extent.x, column, y);
        if (added.r > 0.1f && added.r > 2.0f * added.g)
        {
            imageTop = y;
            break;
        }
    }
    // The board's mirror image is below the floor seen through the gap.
    REQUIRE(imageTop < Extent.y - 4);
    REQUIRE(imageTop > boardBottom + 8);

    // The floor through the gap — behind the board — keeps the colour it has with no reflection
    // at all: its rays reach nothing. The rows next to the image are left out, where the
    // reflection's blur reaches.
    const u32 gapTop = boardBottom + 2;
    const u32 gapBottom = imageTop - 3;
    const u32 x0 = column - 8;
    const u32 x1 = column + 8;
    const vec3 gapPlain = MeanColour(plain, x0, x1, gapTop, gapBottom);
    const vec3 gapTraced = MeanColour(traced, x0, x1, gapTop, gapBottom);
    CHECK(gapTraced.r - gapPlain.r < 0.01f);

    // The mirror image itself is the board's red.
    const vec3 image = MeanColour(traced, x0, x1, imageTop, std::min(imageTop + 4, Extent.y));
    const vec3 imagePlain = MeanColour(plain, x0, x1, imageTop, std::min(imageTop + 4, Extent.y));
    CHECK(image.r - imagePlain.r > 0.05f);

    std::filesystem::remove(archive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "ssr reach: a scene's RenderLook sets the distance its presenting viewport's "
                  "trace reaches")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const path archive = MountPack(assets, "veng_gpu_ssr_reach_look.vengpack");
    const FarWallScene built = BuildFarWallScene(Context, assets, Types);
    auto& look = built.World->Add<RenderLook>(built.World->CreateEntity());
    look.Bloom = false;
    look.Shadows = false;
    look.AO = false;
    look.SSR = true;

    ViewportCompositor compositor(Context);
    const Unique<Viewport> viewport = Viewport::Create({
        .Context = Context,
        .Assets = assets,
        .Region = {.Offset = {0, 0}, .Extent = Extent},
        .ColorFormat = Format::RGBA16Sfloat,
        .Role = ViewportRole::Offscreen,
    });
    compositor.RegisterViewport(*viewport);

    // The pushed view carries only the scene and camera; the look supplies every SSR knob.
    const auto Render = [&]() -> vector<u8>
    {
        for (u32 frame = 0; frame < 2; ++frame)
        {
            viewport->SetViewState(
                {.World = built.World.get(), .Camera = built.Camera, .Delta = 0.0f});
            compositor.RenderRegistered(Context.BeginFrame());
            Context.EndFrame();
        }
        Context.WaitIdle();
        return viewport->GetOutput()->GetImage()->Download();
    };

    look.SsrMaxDistance = 50.0f;
    const vector<u8> shortRay = Render();
    CHECK(viewport->GetViewState().SsrMaxDistance == 50.0f);

    look.SsrMaxDistance = 3000.0f;
    look.SsrMaxSteps = 512;
    const vector<u8> longRay = Render();
    CHECK(viewport->GetViewState().SsrMaxSteps == 512u);

    CHECK(Luma(ReflectionBand(longRay)) > Luma(ReflectionBand(shortRay)) + 0.02f);

    std::filesystem::remove(archive);
}

namespace
{
    // white_plane_pack: the normal-mapped metal (its NormalStrength 0 makes it a flat mirror) and
    // the brick material, the parent of an unlit emitter.
    constexpr AssetId MirrorInstanceId{0xBCAE43829219AE51ULL};
    constexpr AssetId EmitterMaterial{0x000000000000232BULL};

    constexpr uvec2 WideExtent{512, 288};

    // The summed luminance of an RGBA16F target at WideExtent.
    f64 SumLuma(const vector<u8>& rgba16f)
    {
        const auto* halves = reinterpret_cast<const u16*>(rgba16f.data());
        f64 sum = 0.0;
        for (usize i = 0; i < static_cast<usize>(WideExtent.x) * WideExtent.y; ++i)
        {
            sum += Luma(vec3(glm::unpackHalf1x16(halves[i * 4 + 0]),
                             glm::unpackHalf1x16(halves[i * 4 + 1]),
                             glm::unpackHalf1x16(halves[i * 4 + 2])));
        }
        return sum;
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "ssr reach: a mirror seen from just above it reflects an emitter at range as "
                  "fully as one nearby")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const path archive = Veng::TestSupport::TempDir() / "veng_gpu_ssr_reach_emitter.vengpack";
    {
        Cook::Cooker cooker;
        Cook::RegisterBuiltinImporters(cooker);
        REQUIRE(Veng::TestSupport::CookCached(
                    cooker, path(GPU_GBUFFER_FIXTURE_DIR) / "white_plane_pack.json", archive, {},
                    nullptr, nullptr, nullptr, nullptr, {}, path(VENG_CORE_SHADER_DIR))
                    .has_value());
    }
    REQUIRE(assets.Mount(archive).has_value());
    const AssetResult<AssetHandle<MaterialInstance>> mirror =
        assets.LoadSync<MaterialInstance>(MirrorInstanceId);
    REQUIRE(mirror.has_value());
    auto& mirrorParams = const_cast<MaterialInstance&>(*mirror->Get());
    mirrorParams.SetParam("MetallicFactor", 1.0f);
    mirrorParams.SetParam("RoughnessFactor", 0.05f);
    mirrorParams.SetParam("NormalStrength", 0.0f);
    const AssetResult<AssetHandle<Material>> emitterParent =
        assets.LoadSync<Material>(EmitterMaterial);
    REQUIRE(emitterParent.has_value());
    const AssetHandle<MaterialInstance> emitter = assets.BuildSync<MaterialInstance>(
        MaterialInstanceInfo{.Name = "Emitter", .Context = &Context, .Parent = *emitterParent});
    REQUIRE(emitter.IsLoaded());
    auto& emitterParams = const_cast<MaterialInstance&>(*emitter.Get());
    emitterParams.SetParam("BaseColorFactor", vec4(0.0f, 0.0f, 0.0f, 1.0f));
    emitterParams.SetParam("EmissiveFactor", vec4(2.0f, 2.0f, 2.0f, 0.0f));

    // The eye 0.3 m above a white metal mirror, looking 10 degrees down; a square emitter 6
    // degrees across facing the eye, 6 degrees above its line of sight's horizontal. No light
    // and no ambient term: everything the mirror shows is reflection. The rays toward a distant
    // emitter leave the mirror a little ahead of the eye, so on screen each runs into its
    // vanishing point, and every depth past about a hundred metres falls in its last texel.
    const vec3 eye(0.0f, 0.3f, 0.0f);
    const f32 elevation = glm::radians(6.0f);
    const vec3 sight(0.0f, std::sin(elevation), -std::cos(elevation));
    const quat facing = glm::angleAxis(glm::radians(90.0f) + elevation, vec3(1.0f, 0.0f, 0.0f));
    CameraView camera;
    camera.SetPerspective(glm::radians(70.0f),
                          static_cast<f32>(WideExtent.x) / static_cast<f32>(WideExtent.y), 0.05f,
                          2000.0f);
    const f32 pitch = glm::radians(-10.0f);
    camera.SetView(eye, eye + vec3(0.0f, std::sin(pitch), -std::cos(pitch)),
                   vec3(0.0f, 1.0f, 0.0f));

    SceneRendererSettings settings{.Mode = DebugView::Final,
                                   .Bloom = false,
                                   .Shadows = false,
                                   .PunctualShadows = false,
                                   .SSR = true,
                                   .AO = false};
    settings.AntiAliasing = AntiAliasingMode::None;
    settings.SsrMethod = SceneRendererSettings::SsrTechnique::Stochastic;
    const Unique<SceneRenderer> renderer =
        SceneRenderer::Create({.Context = Context,
                               .Assets = assets,
                               .OutputFormat = Context.GetOutputFormat(),
                               .Extent = WideExtent,
                               .Settings = settings});

    // The share of the emitter's energy its reflection carries, over what a mirror carries:
    // the solid angle the emitter fills seen from the eye's mirror image, over seen from the eye.
    // The intersection's radiance, premultiplied by its confidence, is the reflection a white
    // metal mirror adds; the lit scene beside it, which nothing composites into yet, is the
    // emitter seen directly.
    const auto ReflectedShare = [&](const f32 distance) -> f64
    {
        const Unique<Scene> scene = Scene::Create(Types);
        const Ref<Mesh> floor = Mesh::BuildSync(
            Context, Primitives::Plane(vec2(10.0f), uvec2(1), *mirror), "Mirror Floor");
        const f32 side = 2.0f * distance * std::tan(glm::radians(3.0f));
        const Ref<Mesh> panel =
            Mesh::BuildSync(Context, Primitives::Plane(vec2(side), uvec2(1), emitter), "Emitter");
        const Entity floorEntity = scene->CreateEntity();
        scene->Add<Transform>(floorEntity).Position = vec3(0.0f, 0.0f, -4.5f);
        scene->Add<MeshRenderer>(floorEntity).Mesh = assets.Adopt(floor);
        const vec3 centre = eye + sight * distance;
        const Entity panelEntity = scene->CreateEntity();
        auto& transform = scene->Add<Transform>(panelEntity);
        transform.Position = centre;
        transform.Rotation = facing;
        scene->Add<MeshRenderer>(panelEntity).Mesh = assets.Adopt(panel);

        Context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                renderer->Execute(cmd, Renderer::SceneView{.World = *scene,
                                                           .Camera = camera,
                                                           .Delta = 0.0f,
                                                           .AmbientFloor = vec3(0.0f)});
            });
        const f64 direct = SumLuma(renderer->GetHdrView()->GetImage()->Download());
        const f64 reflected =
            SumLuma(renderer->GetSsrTraceTargets().Radiance->GetImage()->Download());

        const vec3 normal = facing * vec3(0.0f, 1.0f, 0.0f);
        const auto solidAngle = [&](const vec3& from)
        {
            const vec3 toward = from - centre;
            const f32 length = glm::length(toward);
            return std::abs(glm::dot(normal, toward)) / (length * length * length);
        };
        const f64 mirrorShare = solidAngle(vec3(eye.x, -eye.y, eye.z)) / solidAngle(eye);
        REQUIRE(direct > 0.0);
        return reflected / (direct * mirrorShare);
    };

    const f64 near = ReflectedShare(30.0f);
    const f64 far = ReflectedShare(300.0f);
    MESSAGE("reflected share at 30 m " << near << ", at 300 m " << far);
    // Near, the reflection carries most of what a mirror would (the screen-edge fade and the
    // texels the emitter's edges straddle take the rest); far away, as much again.
    CHECK(near > 0.7);
    CHECK(far > 0.7);
    CHECK(far > 0.85 * near);

    std::filesystem::remove(archive);
}
