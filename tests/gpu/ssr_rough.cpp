// Rough screen-space reflections blur by the reflected cone, smoothly. Five properties:
//
//  - a small bright emitter reflected in a rough floor keeps its energy and the shape of its blur
//    as it moves by fractions of a pixel: the rough reflection carries the trace's own (mirror)
//    energy within a few percent at every shift, and its peak against its total holds, so a
//    reflection does not step from one coarse texel to the next;
//  - a rough reflection of a bright wall carries no texel-grid structure: its profile across the
//    wall's blurred edge has a bounded second difference against its own magnitude;
//  - the apparent blur of a reflected edge is the same at SSR resolution Full and Half;
//  - at one roughness, the reflection of a wall's side edge is narrower where the wall meets the
//    floor than where the reflected ray travels far;
//  - a mirror floor stays sharp — as sharp as the trace at Full SSR resolution — and a reflection
//    fades out smoothly as its hits near the screen's edge.
//
// Every measure is on the linear HDR target the SSR composite writes, as the difference between a
// renderer with SSR and one without, so the floor's own shading cancels. The floor is a white
// metal, whose Fresnel term is one: the difference is the reflection itself. Skips cleanly (exit
// 77) on a machine with no Vulkan ICD.

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
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include <gpu/fixture.h>

#include "support/TempPath.h"
#include "support/TestCook.h"

#include <algorithm>
#include <cmath>

// glm's packing header after Veng.h, which configures glm first.
#include <glm/gtc/packing.hpp>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    // forward_lighting_pack's opaque material: BaseColor, Roughness, Metallic, Occlusion, untextured.
    constexpr AssetId OpaqueMaterial{0x3AEA87F2E94072A6ULL};

    constexpr uvec2 Extent{128, 128};

    // The luminance of every texel of an RGBA16F image, row-major.
    vector<f32> DecodeLuma(const vector<u8>& rgba16f)
    {
        const auto* halves = reinterpret_cast<const u16*>(rgba16f.data());
        vector<f32> luma(static_cast<usize>(Extent.x) * Extent.y);
        for (usize i = 0; i < luma.size(); ++i)
        {
            const vec3 c(glm::unpackHalf1x16(halves[i * 4 + 0]),
                         glm::unpackHalf1x16(halves[i * 4 + 1]),
                         glm::unpackHalf1x16(halves[i * 4 + 2]));
            luma[i] = glm::dot(c, vec3(0.2126f, 0.7152f, 0.0722f));
        }
        return luma;
    }

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

    // A white metal floor at y = 0 of the given roughness, lit by a directional light from behind
    // the camera, which stands 0.9 m up looking down -Z.
    struct RoughScene
    {
        Unique<Scene> World;
        vector<Ref<Mesh>> Meshes;
        CameraView Camera;
        Entity Subject;
        AssetHandle<MaterialInstance> Floor;
    };

    RoughScene BuildFloor(Context& context, AssetManager& assets, TypeRegistry& types,
                          const f32 floorRoughness)
    {
        RoughScene built{
            .World = Scene::Create(types),
            .Floor = MakeMaterial(context, assets, "Floor", vec3(1.0f), floorRoughness, 1.0f)};
        Scene& scene = *built.World;
        const Ref<Mesh> floor = Mesh::BuildSync(
            context, Primitives::Plane(vec2(40.0f), uvec2(1), built.Floor), "Rough Floor");
        built.Meshes.push_back(floor);
        const Entity floorEntity = scene.CreateEntity();
        scene.Add<Transform>(floorEntity);
        scene.Add<MeshRenderer>(floorEntity).Mesh = assets.Adopt(floor);

        const Entity light = scene.CreateEntity();
        scene.Add<Light>(light) = Light{
            .Type = LightType::Directional,
            .Direction = glm::normalize(vec3(0.0f, -0.4f, -1.0f)),
            .Color = vec3(1.0f),
            .Intensity = 8.0f / LuminousAnchor,
        };

        built.Camera.SetPerspective(glm::radians(60.0f), 1.0f, 0.05f, 100.0f);
        built.Camera.SetView(vec3(0.0f, 0.9f, 2.2f), vec3(0.0f, 0.1f, -1.0f),
                             vec3(0.0f, 1.0f, 0.0f));
        return built;
    }

    // A bright matte wall 1.6 m wide and `height` tall standing on the floor 1 m down -Z.
    RoughScene BuildWallScene(Context& context, AssetManager& assets, TypeRegistry& types,
                              const f32 floorRoughness, const f32 height)
    {
        RoughScene built = BuildFloor(context, assets, types, floorRoughness);
        const Ref<Mesh> wall = Mesh::BuildSync(
            context,
            Primitives::Plane(vec2(1.6f, height), uvec2(1),
                              MakeMaterial(context, assets, "Wall", vec3(0.9f), 1.0f, 0.0f)),
            "Wall");
        built.Meshes.push_back(wall);
        built.Subject = built.World->CreateEntity();
        auto& transform = built.World->Add<Transform>(built.Subject);
        transform.Position = vec3(0.0f, height * 0.5f, -1.0f);
        transform.Rotation = glm::angleAxis(glm::radians(90.0f), vec3(1.0f, 0.0f, 0.0f));
        built.World->Add<MeshRenderer>(built.Subject).Mesh = assets.Adopt(wall);
        return built;
    }

    // A small bright sphere floating half a metre above the floor. The light from behind the
    // camera strikes the face the camera sees head-on, so the scene colour the reflection samples
    // falls to zero at the sphere's silhouette: a reflected ray landing on its edge carries almost
    // no energy, and the reflection's energy does not step as the sphere's coverage of the trace's
    // texels changes.
    RoughScene BuildEmitterScene(Context& context, AssetManager& assets, TypeRegistry& types,
                                 const f32 floorRoughness)
    {
        RoughScene built = BuildFloor(context, assets, types, floorRoughness);
        const vec3 centre(0.0f, 0.5f, -0.4f);
        const Ref<Mesh> sphere = Mesh::BuildSync(
            context,
            Primitives::Sphere(0.1f, 32, 64,
                               MakeMaterial(context, assets, "Emitter", vec3(1.0f), 1.0f, 0.0f)),
            "Emitter");
        built.Meshes.push_back(sphere);
        built.Subject = built.World->CreateEntity();
        built.World->Add<Transform>(built.Subject).Position = centre;
        built.World->Add<MeshRenderer>(built.Subject).Mesh = assets.Adopt(sphere);
        return built;
    }

    // Two renderers over one scene, one with SSR and one without, at one SSR resolution.
    struct ReflectionProbe
    {
        Unique<SceneRenderer> Traced;
        Unique<SceneRenderer> Plain;

        ReflectionProbe(Context& context, AssetManager& assets,
                        const SceneRendererSettings::SsrResolution resolution)
        {
            SceneRendererSettings settings{.Mode = DebugView::Final,
                                           .Bloom = false,
                                           .Shadows = false,
                                           .SSR = true,
                                           .AO = false};
            settings.SsrResolutionScale = resolution;
            Traced = SceneRenderer::Create({.Context = context,
                                            .Assets = assets,
                                            .OutputFormat = context.GetOutputFormat(),
                                            .Extent = Extent,
                                            .Settings = settings});
            settings.SSR = false;
            Plain = SceneRenderer::Create({.Context = context,
                                           .Assets = assets,
                                           .OutputFormat = context.GetOutputFormat(),
                                           .Extent = Extent,
                                           .Settings = settings});
        }

        // The luminance the reflection adds to each texel, and the plain render's luminance.
        std::pair<vector<f32>, vector<f32>> Render(Context& context, const RoughScene& scene)
        {
            context.ImmediateCommands(
                [&](CommandBuffer& cmd)
                {
                    const Renderer::SceneView view{.World = *scene.World,
                                                   .Camera = scene.Camera,
                                                   .Delta = 0.0f,
                                                   .SsrMaxDistance = 20.0f};
                    Traced->Execute(cmd, view);
                    Plain->Execute(cmd, view);
                });
            const vector<f32> traced = DecodeLuma(Traced->GetHdrView()->GetImage()->Download());
            vector<f32> plain = DecodeLuma(Plain->GetHdrView()->GetImage()->Download());
            vector<f32> added(traced.size());
            for (usize i = 0; i < added.size(); ++i)
            {
                added[i] = traced[i] - plain[i];
            }
            return {std::move(added), std::move(plain)};
        }
    };

    f32 At(const vector<f32>& image, const u32 x, const u32 y)
    {
        return image[static_cast<usize>(y) * Extent.x + x];
    }

    // The mean of a few columns centred on `x`, down every row.
    vector<f32> ColumnProfile(const vector<f32>& image, const u32 x)
    {
        vector<f32> profile(Extent.y, 0.0f);
        for (u32 y = 0; y < Extent.y; ++y)
        {
            for (u32 dx = 0; dx < 4; ++dx)
            {
                profile[y] += At(image, x - 2 + dx, y) * 0.25f;
            }
        }
        return profile;
    }

    // The mean of three rows centred on `y`, across every column.
    vector<f32> RowProfile(const vector<f32>& image, const u32 y)
    {
        vector<f32> profile(Extent.x, 0.0f);
        for (u32 x = 0; x < Extent.x; ++x)
        {
            for (u32 dy = 0; dy < 3; ++dy)
            {
                profile[x] += At(image, x, y - 1 + dy) / 3.0f;
            }
        }
        return profile;
    }

    // Where, walking from `from` toward `to`, the profile first reaches `level`, interpolated.
    f32 Crossing(const vector<f32>& profile, const i32 from, const i32 to, const f32 level)
    {
        const i32 step = to > from ? 1 : -1;
        for (i32 i = from; i != to; i += step)
        {
            const f32 a = profile[static_cast<usize>(i)];
            const f32 b = profile[static_cast<usize>(i + step)];
            if (a < level && b >= level)
            {
                return static_cast<f32>(i) + static_cast<f32>(step) * (level - a) / (b - a);
            }
        }
        return static_cast<f32>(to);
    }

    // The 20%-80% width of an edge whose dark side is at `dark` and bright side toward `bright`,
    // against the given plateau.
    f32 EdgeWidth(const vector<f32>& profile, const i32 dark, const i32 bright, const f32 plateau)
    {
        return std::abs(Crossing(profile, dark, bright, 0.8f * plateau) -
                        Crossing(profile, dark, bright, 0.2f * plateau));
    }

    // The wall's contact row: the last row down the centre column the plain render shows the wall.
    u32 ContactRow(const vector<f32>& plain)
    {
        const vector<f32> profile = ColumnProfile(plain, Extent.x / 2);
        const f32 wall = *std::ranges::max_element(profile);
        u32 contact = 0;
        for (u32 y = 0; y < Extent.y; ++y)
        {
            if (profile[y] > 0.5f * wall)
            {
                contact = y;
            }
        }
        return contact;
    }

    // The vertical 20%-80% width of a reflected wall's far edge, the bottom of its reflection.
    f32 ReflectedTopEdgeWidth(const vector<f32>& added, const vector<f32>& plain)
    {
        const u32 contact = ContactRow(plain);
        const vector<f32> profile = ColumnProfile(added, Extent.x / 2);
        const f32 plateau = *std::max_element(profile.begin() + contact + 1, profile.end());
        REQUIRE(plateau > 0.0f);
        return EdgeWidth(profile, static_cast<i32>(Extent.y) - 1, static_cast<i32>(contact) + 1,
                         plateau);
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "ssr rough: a small emitter's rough reflection keeps its energy and shape under "
                  "sub-pixel motion")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const path archive = MountPack(assets, "veng_gpu_ssr_rough_emitter.vengpack");
    constexpr f32 Roughness = 0.4f;
    const RoughScene built = BuildEmitterScene(Context, assets, Types, Roughness);
    ReflectionProbe probe(Context, assets, SceneRendererSettings::SsrResolution::Full);
    auto& floor = const_cast<MaterialInstance&>(*built.Floor.Get());

    // The trace takes one ray per texel, so how much of a small emitter it hits steps with the
    // emitter's sub-texel position whatever filters it afterwards; the blur's own claim is that it
    // neither gains nor loses light, nor changes shape, wherever the reflection falls on the grid.
    // So each shift renders the mirror too, the trace's own unblurred result, and the rough
    // reflection's energy is measured against it. Three-quarter-pixel steps sweep a whole texel of
    // the level the reflection reads.
    const f32 pixel = 2.0f * 2.9f * std::tan(glm::radians(30.0f)) / static_cast<f32>(Extent.y);
    constexpr u32 Shifts = 12;
    f32 ratioMin = 1e30f;
    f32 ratioMax = 0.0f;
    f32 shapeMin = 1e30f;
    f32 shapeMax = 0.0f;
    for (u32 shift = 0; shift < Shifts; ++shift)
    {
        built.World->Get<Transform>(built.Subject).Position.x =
            pixel * 0.75f * static_cast<f32>(shift);

        // The reflection lies below the frame's middle row; the emitter's own image is above it.
        const auto Reflected = [&](const f32 roughness) -> std::pair<f32, f32>
        {
            floor.SetParam("Roughness", roughness);
            const vector<f32> added = probe.Render(Context, built).first;
            f32 total = 0.0f;
            f32 peak = 0.0f;
            for (u32 y = Extent.y / 2; y < Extent.y; ++y)
            {
                for (u32 x = 0; x < Extent.x; ++x)
                {
                    const f32 v = std::max(At(added, x, y), 0.0f);
                    total += v;
                    peak = std::max(peak, v);
                }
            }
            return {total, peak};
        };
        const auto [mirror, mirrorPeak] = Reflected(0.0f);
        const auto [rough, roughPeak] = Reflected(Roughness);
        REQUIRE(mirror > 0.0f);
        REQUIRE(rough > 0.0f);
        // The rough reflection is a blur, not a mirror.
        REQUIRE(roughPeak < 0.5f * mirrorPeak);

        const f32 ratio = rough / mirror;
        const f32 shape = roughPeak / rough;
        ratioMin = std::min(ratioMin, ratio);
        ratioMax = std::max(ratioMax, ratio);
        shapeMin = std::min(shapeMin, shape);
        shapeMax = std::max(shapeMax, shape);
    }

    CHECK(ratioMin > 0.9f);
    CHECK(ratioMax < 1.05f);
    CHECK((ratioMax - ratioMin) / ratioMax < 0.05f);
    CHECK((shapeMax - shapeMin) / shapeMax < 0.12f);

    std::filesystem::remove(archive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "ssr rough: a rough reflection of a bright wall carries no texel grid")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const path archive = MountPack(assets, "veng_gpu_ssr_rough_grid.vengpack");
    const RoughScene built = BuildWallScene(Context, assets, Types, 0.45f, 1.2f);
    ReflectionProbe probe(Context, assets, SceneRendererSettings::SsrResolution::Half);
    const auto [added, plain] = probe.Render(Context, built);

    // Down the centre column, through the reflection's blurred far edge, the profile's second
    // difference stays small against the reflection's own magnitude. Bilinear magnification of a
    // coarse texel grid would kink it at every texel boundary. The rows just below the contact line
    // are left out: the reflection is sharp where the wall meets the floor, by design.
    const u32 contact = ContactRow(plain);
    const vector<f32> profile = ColumnProfile(added, Extent.x / 2);
    const f32 plateau = *std::max_element(profile.begin() + contact + 1, profile.end());
    REQUIRE(plateau > 0.0f);
    f32 worst = 0.0f;
    for (u32 y = contact + 8; y + 1 < Extent.y; ++y)
    {
        worst = std::max(worst, std::abs(profile[y - 1] - 2.0f * profile[y] + profile[y + 1]));
    }
    CHECK(worst < 0.03f * plateau);

    std::filesystem::remove(archive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "ssr rough: a reflected edge blurs by the same width at Full and Half SSR "
                  "resolution")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const path archive = MountPack(assets, "veng_gpu_ssr_rough_resolution.vengpack");
    const RoughScene built = BuildWallScene(Context, assets, Types, 0.3f, 1.2f);

    ReflectionProbe full(Context, assets, SceneRendererSettings::SsrResolution::Full);
    ReflectionProbe half(Context, assets, SceneRendererSettings::SsrResolution::Half);
    const auto [fullAdded, fullPlain] = full.Render(Context, built);
    const auto [halfAdded, halfPlain] = half.Render(Context, built);

    const f32 fullWidth = ReflectedTopEdgeWidth(fullAdded, fullPlain);
    const f32 halfWidth = ReflectedTopEdgeWidth(halfAdded, halfPlain);
    // The edge is visibly blurred, and by the same amount in output pixels at either resolution.
    CHECK(fullWidth > 4.0f);
    CHECK(std::abs(fullWidth - halfWidth) < 0.2f * fullWidth);

    std::filesystem::remove(archive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "ssr rough: a reflection blurs less where the reflected ray is short")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const path archive = MountPack(assets, "veng_gpu_ssr_rough_distance.vengpack");
    const RoughScene built = BuildWallScene(Context, assets, Types, 0.3f, 1.2f);
    ReflectionProbe probe(Context, assets, SceneRendererSettings::SsrResolution::Half);
    const auto [added, plain] = probe.Render(Context, built);

    // The wall's right edge, reflected: a few rows below the contact line its rays travel a few
    // centimetres; further down, toward the reflection of the wall's top, over a metre.
    const u32 contact = ContactRow(plain);
    const vector<f32> column = ColumnProfile(added, Extent.x / 2);
    const f32 bottom =
        Crossing(column, static_cast<i32>(Extent.y) - 1, static_cast<i32>(contact) + 1,
                 0.5f * *std::max_element(column.begin() + contact + 1, column.end()));
    const u32 nearRow = contact + 3;
    const u32 farRow = contact + static_cast<u32>((bottom - static_cast<f32>(contact)) * 0.7f);
    REQUIRE(farRow > nearRow + 6);

    const auto SideEdgeWidth = [&](const u32 row)
    {
        const vector<f32> profile = RowProfile(added, row);
        const f32 plateau = profile[Extent.x / 2];
        REQUIRE(plateau > 0.0f);
        return EdgeWidth(profile, static_cast<i32>(Extent.x) - 1, static_cast<i32>(Extent.x / 2),
                         plateau);
    };
    const f32 nearWidth = SideEdgeWidth(nearRow);
    const f32 farWidth = SideEdgeWidth(farRow);
    CHECK(nearWidth < 0.6f * farWidth);

    std::filesystem::remove(archive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "ssr rough: a mirror stays sharp and its reflection fades out near the screen "
                  "edge")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const path archive = MountPack(assets, "veng_gpu_ssr_rough_mirror.vengpack");
    ReflectionProbe probe(Context, assets, SceneRendererSettings::SsrResolution::Half);

    // At one composite pixel per SSR texel the mirror's reflected edge is as sharp as the trace
    // draws it; at Half, magnified through the B-spline, it spans little more than one SSR texel.
    {
        const RoughScene built = BuildWallScene(Context, assets, Types, 0.0f, 1.2f);
        ReflectionProbe full(Context, assets, SceneRendererSettings::SsrResolution::Full);
        const auto [fullAdded, fullPlain] = full.Render(Context, built);
        CHECK(ReflectedTopEdgeWidth(fullAdded, fullPlain) < 1.5f);
        const auto [added, plain] = probe.Render(Context, built);
        CHECK(ReflectedTopEdgeWidth(added, plain) < 2.5f);
    }

    // A wall rising past the top of the frame: the floor's bottom rows reflect its part beyond the
    // screen and add nothing, and between those and the plateau the reflection fades over rows
    // rather than cutting off.
    {
        const RoughScene built = BuildWallScene(Context, assets, Types, 0.0f, 6.0f);
        const auto [added, plain] = probe.Render(Context, built);
        const u32 contact = ContactRow(plain);
        const vector<f32> profile = ColumnProfile(added, Extent.x / 2);
        const f32 plateau = *std::max_element(profile.begin() + contact + 1, profile.end());
        REQUIRE(plateau > 0.0f);
        CHECK(profile[Extent.y - 1] < 0.1f * plateau);
        u32 fading = 0;
        for (u32 y = contact + 1; y < Extent.y; ++y)
        {
            fading += profile[y] > 0.1f * plateau && profile[y] < 0.9f * plateau ? 1u : 0u;
        }
        CHECK(fading >= 4u);
    }

    std::filesystem::remove(archive);
}
