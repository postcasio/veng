// SpriteScenePass on a real device: a FlipbookSprite over a black, empty scene composites the
// flipbook's frame into the lit scene color. A white premultiplied atlas makes the expected pixel the
// tint itself, so an alpha sprite shows its tint and two overlapping additive sprites show their sum;
// a sprite-less scene carries no pass and renders black, and a sprite too dim for the bloom threshold
// still lights the margin around it, which only its bloom-mask write can do. An area light reaches a
// six-way sprite as a small source of its power.
//
// A schema-1 sprite stays unlit, stepped and hard-edged by default. A six-way flipbook built in code
// — light only in lightA's +X map — reads the light arriving along +X at the white-card
// normalisation and none from -X, loses it behind an opaque caster, glows through its emission ramp
// only above ignition, and fades into an opaque wall over its soft distance.
//
// Under TAA a sprite appearing over a settled background shows at once rather than fading in
// through the history: it writes its coverage into the reactive mask the resolve reads.

#include <array>
#include <cmath>
#include <functional>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/gtc/packing.hpp>
#include <glm/gtx/quaternion.hpp>

#include <doctest/doctest.h>

#include <Veng/Asset/AssetBuild.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Flipbook.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Primitives.h>
#include <Veng/Asset/Texture.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Renderer/CommandBuffer.h>
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

// glm's constants after Veng.h, which configures glm first.
#include <glm/gtc/constants.hpp>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr uvec2 Extent{64, 64};

    // A 2x2 grid of 4x4 white, fully covered frames: sampled anywhere it reads (1, 1, 1, 1).
    AssetHandle<Flipbook> WhiteFlipbook(Context& context, const FlipbookBlend blend)
    {
        std::array<u8, 8 * 8 * 4> pixels{};
        pixels.fill(255);
        const Ref<Texture> atlas = Veng::Detail::BuildAssetSync(
            context, TextureData{
                         .Name = "Sprite Atlas",
                         .Extent = {8, 8},
                         .Format = Format::RGBA8Unorm,
                         .Pixels = pixels,
                         .Sampler = {.AddressModeU = AddressMode::ClampToEdge,
                                     .AddressModeV = AddressMode::ClampToEdge,
                                     .AddressModeW = AddressMode::ClampToEdge},
                     });
        return AssetManager::Adopt(Flipbook::Create(FlipbookInfo{
            .Name = "White",
            .Atlas = atlas,
            .Columns = 2,
            .Rows = 2,
            .FrameSize = {4, 4},
            .Clip = {.FrameCount = 4, .Fps = 10.0f, .Loop = true},
            .Blend = blend,
            .AlphaMode = FlipbookAlpha::Premultiplied,
        }));
    }

    vec3 CenterRgb(const std::vector<u8>& rgba16f)
    {
        const auto* halves = reinterpret_cast<const u16*>(rgba16f.data());
        const usize base = (static_cast<usize>(Extent.y / 2) * Extent.x + Extent.x / 2) * 4;
        return vec3(glm::unpackHalf1x16(halves[base + 0]), glm::unpackHalf1x16(halves[base + 1]),
                    glm::unpackHalf1x16(halves[base + 2]));
    }

    struct SpriteRender
    {
        Unique<SceneRenderer> Renderer;
        CameraView Camera;

        std::vector<u8> Render(Context& context, const Scene& scene)
        {
            context.ImmediateCommands(
                [&](CommandBuffer& cmd)
                {
                    Renderer->Execute(cmd,
                                      Veng::Renderer::SceneView{.World = scene,
                                                                .Camera = Camera,
                                                                .Delta = 0.0f,
                                                                .Exposure = 1.0f,
                                                                .Tonemapper = Tonemapper::None});
                });
            return Renderer->GetOutput()->GetImage()->Download();
        }
    };

    SpriteRender MakeRenderer(Context& context, AssetManager& assets, const bool bloom,
                              const bool shadows = false)
    {
        SpriteRender render{
            .Renderer = SceneRenderer::Create({
                .Context = context,
                .Assets = assets,
                .OutputFormat = context.GetOutputFormat(),
                .Extent = Extent,
                .Settings = {.Bloom = bloom, .Shadows = shadows, .AO = false},
            }),
        };
        render.Camera.SetPerspective(glm::radians(50.0f), 1.0f, 0.1f, 100.0f);
        render.Camera.SetView(vec3(0.0f, 0.0f, -5.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));
        return render;
    }

    Entity AddSprite(Scene& scene, const AssetHandle<Flipbook>& flipbook, const SpriteBlend blend,
                     const vec3 tint, const vec3 position)
    {
        const Entity entity = scene.CreateEntity();
        scene.Add<Transform>(entity, Transform{.Position = position});
        scene.Add<FlipbookSprite>(
            entity,
            FlipbookSprite{.Flipbook = flipbook, .Size = 2.0f, .Tint = tint, .Blend = blend});
        return entity;
    }

    Ref<Texture> SolidTexture(Context& context, const char* name, const uvec2 extent,
                              const std::function<vec4(u32 x, u32 y)>& texel)
    {
        std::vector<u16> halves(static_cast<usize>(extent.x) * extent.y * 4);
        for (u32 y = 0; y < extent.y; ++y)
        {
            for (u32 x = 0; x < extent.x; ++x)
            {
                const vec4 value = texel(x, y);
                const usize base = (static_cast<usize>(y) * extent.x + x) * 4;
                for (usize c = 0; c < 4; ++c)
                {
                    halves[base + c] = glm::packHalf1x16(value[static_cast<int>(c)]);
                }
            }
        }
        return Veng::Detail::BuildAssetSync(
            context, TextureData{
                         .Name = name,
                         .Extent = extent,
                         .Format = Format::RGBA16Sfloat,
                         .Pixels = std::span(reinterpret_cast<const u8*>(halves.data()),
                                             halves.size() * sizeof(u16)),
                         .Sampler = {.AddressModeU = AddressMode::ClampToEdge,
                                     .AddressModeV = AddressMode::ClampToEdge,
                                     .AddressModeW = AddressMode::ClampToEdge},
                     });
    }

    // A schema-1-shaped flipbook of two 4x4 frames: frame 0 opaque red, frame 1 opaque green.
    AssetHandle<Flipbook> RedGreenFlipbook(Context& context)
    {
        const Ref<Texture> atlas = SolidTexture(
            context, "Red Green Atlas", {8, 4}, [](const u32 x, u32)
            { return x < 4 ? vec4(1.0f, 0.0f, 0.0f, 1.0f) : vec4(0.0f, 1.0f, 0.0f, 1.0f); });
        return AssetManager::Adopt(Flipbook::Create(FlipbookInfo{
            .Name = "Red Green",
            .Atlas = atlas,
            .Columns = 2,
            .Rows = 1,
            .FrameSize = {4, 4},
            .Clip = {.FrameCount = 2, .Fps = 10.0f, .Loop = true},
            .AlphaMode = FlipbookAlpha::Premultiplied,
        }));
    }

    // A one-frame six-way flipbook lit only through lightA's +X map, fully covered, with a uniform
    // stored temperature and the given default ramp.
    AssetHandle<Flipbook> SixWayFlipbook(Context& context, const f32 storedTemperature,
                                         const FlipbookEmission& emission)
    {
        const Ref<Texture> lightA = SolidTexture(context, "Six-Way Light A", {4, 4}, [](u32, u32)
                                                 { return vec4(1.0f, 0.0f, 0.0f, 1.0f); });
        const Ref<Texture> lightB =
            SolidTexture(context, "Six-Way Light B", {4, 4}, [](u32, u32) { return vec4(0.0f); });
        const Ref<Texture> temperature =
            SolidTexture(context, "Six-Way Temperature", {4, 4},
                         [storedTemperature](u32, u32) { return vec4(storedTemperature); });
        return AssetManager::Adopt(Flipbook::Create(FlipbookInfo{
            .Name = "Six-Way",
            .Atlas = lightA,
            .FrameSize = {4, 4},
            .Clip = {.FrameCount = 1, .Fps = 10.0f, .Loop = true},
            .AlphaMode = FlipbookAlpha::Premultiplied,
            .Shading = FlipbookShading::SixWay,
            .LightB = lightB,
            .Temperature = temperature,
            .TemperatureEncoding = FlipbookTemperatureEncoding::Sqrt,
            .Emission = emission,
            .FrameBlend = FlipbookFrameBlend::Crossfade,
        }));
    }

    // The camera's right axis in world space: the first row of its view matrix.
    vec3 CameraRight(const CameraView& camera)
    {
        const mat4 view = camera.View();
        return glm::normalize(vec3(view[0][0], view[1][0], view[2][0]));
    }

    // A directional light of irradiance e travelling along direction.
    Entity AddSun(Scene& scene, const vec3 direction, const f32 irradiance)
    {
        const Entity sun = scene.CreateEntity();
        scene.Add<Light>(sun) = Light{
            .Type = LightType::Directional,
            .Direction = direction,
            .Color = vec3(1.0f),
            .Intensity = irradiance / LuminousAnchor,
        };
        return sun;
    }

    // The fixture pack's opaque metallic-roughness material, for walls and casters.
    struct OpaquePack
    {
        path Archive;
        AssetHandle<MaterialInstance> Material;

        OpaquePack(AssetManager& assets, const char* archiveName)
            : Archive(Veng::TestSupport::TempDir() / archiveName)
        {
            Cook::Cooker cooker;
            Cook::RegisterBuiltinImporters(cooker);
            REQUIRE(Veng::TestSupport::CookCached(
                        cooker, path(GPU_GBUFFER_FIXTURE_DIR) / "forward_lighting_pack.json",
                        Archive, {}, nullptr, nullptr, nullptr, nullptr, {},
                        path(VENG_CORE_SHADER_DIR))
                        .has_value());
            REQUIRE(assets.Mount(Archive).has_value());
            const AssetResult<AssetHandle<MaterialInstance>> opaque =
                assets.LoadSync<MaterialInstance>(AssetId{0xF34FF2CE488F7B19ULL});
            REQUIRE(opaque.has_value());
            Material = *opaque;
        }

        // An opaque cube of the given edge centred at position.
        Entity AddCube(Context& context, Scene& scene, const f32 edge, const vec3 position) const
        {
            const Entity cube = scene.CreateEntity();
            scene.Add<Transform>(cube).Position = position;
            scene.Add<MeshRenderer>(cube).Mesh = AssetManager::Adopt(
                Mesh::BuildSync(context, Primitives::Cube(edge, Material), "Sprite Wall"));
            return cube;
        }
    };
}

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "sprite pass: an alpha sprite shows its tint; additive sprites add; none is black")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    SpriteRender render = MakeRenderer(Context, assets, /*bloom=*/false);
    const Unique<Scene> scene = Scene::Create(Types);

    // No sprite: the pass is absent and the empty scene renders black.
    CHECK(glm::length(CenterRgb(render.Render(Context, *scene))) < 0.02f);

    // A white premultiplied frame over black, alpha-composited, reads as the tint.
    const AssetHandle<Flipbook> flipbook = WhiteFlipbook(Context, FlipbookBlend::Alpha);
    const Entity alpha =
        AddSprite(*scene, flipbook, SpriteBlend::Alpha, vec3(0.5f, 0.25f, 0.125f), vec3(0.0f));
    {
        const vec3 center = CenterRgb(render.Render(Context, *scene));
        CHECK(center.x == doctest::Approx(0.5f).epsilon(0.02));
        CHECK(center.y == doctest::Approx(0.25f).epsilon(0.02));
        CHECK(center.z == doctest::Approx(0.125f).epsilon(0.02));
    }

    // Two overlapping additive sprites sum, whatever order they draw in.
    scene->DestroyEntity(alpha);
    AddSprite(*scene, flipbook, SpriteBlend::Additive, vec3(0.25f), vec3(0.0f, 0.0f, 0.5f));
    AddSprite(*scene, flipbook, SpriteBlend::Additive, vec3(0.5f), vec3(0.0f, 0.0f, -0.5f));
    {
        const vec3 center = CenterRgb(render.Render(Context, *scene));
        CHECK(center.x == doctest::Approx(0.75f).epsilon(0.02));
        CHECK(center.z == doctest::Approx(0.75f).epsilon(0.02));
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "sprite pass: a dim sprite taking its flipbook's blend glows through the mask")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const Unique<Scene> scene = Scene::Create(Types);
    const AssetHandle<Flipbook> flipbook = WhiteFlipbook(Context, FlipbookBlend::Additive);

    // A pixel in the black margin well clear of the sprite, which only bloom can light.
    const auto halo = [&]()
    {
        SpriteRender render = MakeRenderer(Context, assets, /*bloom=*/true);
        Context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                render.Renderer->Execute(cmd,
                                         Veng::Renderer::SceneView{.World = *scene,
                                                                   .Camera = render.Camera,
                                                                   .Delta = 0.0f,
                                                                   .Exposure = 1.0f,
                                                                   .Tonemapper = Tonemapper::None,
                                                                   .BloomThreshold = 1.0f});
            });
        const std::vector<u8> pixels = render.Renderer->GetOutput()->GetImage()->Download();
        const auto* halves = reinterpret_cast<const u16*>(pixels.data());
        const usize base = (static_cast<usize>(Extent.y / 2) * Extent.x + 4) * 4;
        return glm::unpackHalf1x16(halves[base]);
    };

    const f32 dark = halo();
    // Luminance 0.5, below the threshold: the bright pass alone passes none of it, so a halo is
    // the mask's amplitude.
    AddSprite(*scene, flipbook, SpriteBlend::Asset, vec3(0.5f), vec3(0.0f));
    const f32 lit = halo();
    CHECK(dark < 1e-3f);
    CHECK(lit > dark + 1e-3f);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "sprite pass: a schema-1 sprite is unlit, stepped and hard-edged by default")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const OpaquePack pack(assets, "veng_gpu_sprite_v1.vengpack");
    SpriteRender render = MakeRenderer(Context, assets, /*bloom=*/false);
    const Unique<Scene> scene = Scene::Create(Types);

    const AssetHandle<Flipbook> flipbook = RedGreenFlipbook(Context);
    const Entity entity = AddSprite(*scene, flipbook, SpriteBlend::Alpha, vec3(1.0f), vec3(0.0f));
    const vec3 unlit = CenterRgb(render.Render(Context, *scene));

    // Lit by nothing: a bright light changes nothing.
    const Entity sun = AddSun(*scene, -CameraRight(render.Camera), 20.0f);
    const vec3 underLight = CenterRgb(render.Render(Context, *scene));
    CHECK(glm::length(underLight - unlit) < 1e-3f);
    scene->DestroyEntity(sun);

    // Stepped: halfway through frame 0 it reads frame 0's red, with none of frame 1's green.
    scene->Get<FlipbookSprite>(entity).Time = 0.05f;
    const vec3 halfway = CenterRgb(render.Render(Context, *scene));
    CHECK(halfway.x == doctest::Approx(1.0f).epsilon(0.02));
    CHECK(halfway.y < 0.02f);

    // Hard-edged: just in front of an opaque wall it reads the same full tint as over nothing.
    pack.AddCube(Context, *scene, 4.0f, vec3(0.0f, 0.0f, 2.01f));
    const vec3 overWall = CenterRgb(render.Render(Context, *scene));
    CHECK(glm::length(overWall - halfway) < 1e-3f);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "sprite pass: a six-way sprite is lit by the axis the light arrives on")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    SpriteRender render = MakeRenderer(Context, assets, /*bloom=*/false);
    const Unique<Scene> scene = Scene::Create(Types);
    AddSprite(*scene, SixWayFlipbook(Context, 0.0f, {}), SpriteBlend::Alpha, vec3(1.0f),
              vec3(0.0f));
    const vec3 baseline = CenterRgb(render.Render(Context, *scene));

    // Arriving from the camera's right (+X in the sprite's frame), it lights lightA's red map at
    // the white-card normalisation: E/pi.
    constexpr f32 Irradiance = 2.0f;
    const vec3 right = CameraRight(render.Camera);
    const Entity fromRight = AddSun(*scene, -right, Irradiance);
    const vec3 lit = CenterRgb(render.Render(Context, *scene));
    CHECK(lit.x - baseline.x == doctest::Approx(Irradiance / glm::pi<f32>()).epsilon(0.02));
    scene->DestroyEntity(fromRight);

    // Arriving from the left it reaches only lightB's -X map, which is dark.
    AddSun(*scene, right, Irradiance);
    const vec3 fromLeft = CenterRgb(render.Render(Context, *scene));
    CHECK(glm::length(fromLeft - baseline) < 1e-3f);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "sprite pass: an area light lights a six-way sprite as a source of its power")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    SpriteRender render = MakeRenderer(Context, assets, /*bloom=*/false);
    const Unique<Scene> scene = Scene::Create(Types);
    AddSprite(*scene, SixWayFlipbook(Context, 0.0f, {}), SpriteBlend::Alpha, vec3(1.0f),
              vec3(0.0f));
    const vec3 baseline = CenterRgb(render.Render(Context, *scene));
    const vec3 right = CameraRight(render.Camera);
    constexpr f32 Distance = 20.0f;
    constexpr f32 Luminance = 400.0f;

    // A sphere to the right delivers its luminance over its disc, L pi r^2 / d^2, read at E/pi.
    constexpr f32 SphereRadius = 1.0f;
    const Entity sphere = scene->CreateEntity();
    scene->Add<Transform>(sphere, Transform{.Position = right * Distance});
    scene->Add<Light>(sphere) = Light{.Type = LightType::Sphere,
                                      .Intensity = Luminance / LuminousAnchor,
                                      .Range = 1000.0f,
                                      .Radius = SphereRadius,
                                      .CastsShadows = false};
    {
        const f32 irradiance =
            Luminance * glm::pi<f32>() * SphereRadius * SphereRadius / (Distance * Distance);
        const vec3 lit = CenterRgb(render.Render(Context, *scene));
        CHECK(lit.x - baseline.x == doctest::Approx(irradiance / glm::pi<f32>()).epsilon(0.03));
    }
    scene->DestroyEntity(sphere);

    // A rect facing the sprite delivers its luminance over its area; turned away it delivers none.
    const auto addRect = [&](const vec3 facing)
    {
        const Entity rect = scene->CreateEntity();
        scene->Add<Transform>(rect,
                              Transform{.Position = right * Distance,
                                        .Rotation = glm::rotation(vec3(0.0f, 0.0f, 1.0f), facing)});
        scene->Add<Light>(rect) = Light{.Type = LightType::Rect,
                                        .Intensity = Luminance / LuminousAnchor,
                                        .Range = 1000.0f,
                                        .Width = 2.0f,
                                        .Height = 1.0f,
                                        .CastsShadows = false};
        return rect;
    };
    const Entity toward = addRect(-right);
    {
        const f32 irradiance = Luminance * 2.0f / (Distance * Distance);
        const vec3 lit = CenterRgb(render.Render(Context, *scene));
        CHECK(lit.x - baseline.x == doctest::Approx(irradiance / glm::pi<f32>()).epsilon(0.03));
    }
    scene->DestroyEntity(toward);
    addRect(right);
    CHECK(glm::length(CenterRgb(render.Render(Context, *scene)) - baseline) < 1e-3f);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "sprite pass: an opaque caster between a light and a six-way sprite shadows it")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const OpaquePack pack(assets, "veng_gpu_sprite_shadow.vengpack");
    SpriteRender render = MakeRenderer(Context, assets, /*bloom=*/false, /*shadows=*/true);
    const Unique<Scene> scene = Scene::Create(Types);
    AddSprite(*scene, SixWayFlipbook(Context, 0.0f, {}), SpriteBlend::Alpha, vec3(1.0f),
              vec3(0.0f));
    const vec3 right = CameraRight(render.Camera);

    // The caster sits off to the right, between the +X light and the sprite's centre, wide enough
    // that the cascade's filter kernel lands wholly inside its shadow; the sprite's centre pixel
    // shows the sprite, not the caster.
    const Entity caster = pack.AddCube(Context, *scene, 3.0f, right * 4.0f);
    const vec3 baseline = CenterRgb(render.Render(Context, *scene));
    const Entity sun = AddSun(*scene, -right, 2.0f);
    const vec3 shadowed = CenterRgb(render.Render(Context, *scene));
    CHECK(glm::length(shadowed - baseline) < 1e-3f);

    // Moved out of the light's path, the caster no longer shadows it.
    scene->DestroyEntity(caster);
    pack.AddCube(Context, *scene, 3.0f, vec3(0.0f, 8.0f, 0.0f));
    const vec3 lit = CenterRgb(render.Render(Context, *scene));
    CHECK(lit.x - baseline.x > 0.5f * 2.0f / glm::pi<f32>());
    scene->DestroyEntity(sun);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "sprite pass: a six-way sprite emits through its ramp only above ignition")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    SpriteRender render = MakeRenderer(Context, assets, /*bloom=*/false);
    const Unique<Scene> scene = Scene::Create(Types);

    // The temperature reads 0.6 everywhere, so its screen derivative is 0 and the gate is a step.
    const f32 stored = std::sqrt(0.6f);
    const Entity dark = AddSprite(*scene, SixWayFlipbook(Context, stored, {}), SpriteBlend::Alpha,
                                  vec3(1.0f), vec3(0.0f));
    const vec3 baseline = CenterRgb(render.Render(Context, *scene));
    scene->DestroyEntity(dark);

    // Half a unit of emission, so the output, which clips at 1, carries all of it.
    const FlipbookEmission emission{
        .Ramp = {FlipbookEmissionKey{.T = 0.5f, .Color = vec3(0.5f, 0.0f, 0.0f)}},
        .Ignition = 0.5f};
    const Entity hot = AddSprite(*scene, SixWayFlipbook(Context, stored, emission),
                                 SpriteBlend::Alpha, vec3(1.0f), vec3(0.0f));
    const vec3 glowing = CenterRgb(render.Render(Context, *scene));
    CHECK(glowing.x - baseline.x == doctest::Approx(0.5f).epsilon(0.02));

    // The sprite's own ignition and its temperature offset are both read.
    scene->Get<FlipbookSprite>(hot).Ignition = 0.8f;
    CHECK(glm::length(CenterRgb(render.Render(Context, *scene)) - baseline) < 1e-3f);
    scene->Get<FlipbookSprite>(hot).Ignition = -1.0f;
    scene->Get<FlipbookSprite>(hot).TemperatureOffset = -0.2f;
    CHECK(glm::length(CenterRgb(render.Render(Context, *scene)) - baseline) < 1e-3f);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "sprite pass: a soft sprite fades monotonically into the geometry behind it")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const OpaquePack pack(assets, "veng_gpu_sprite_soft.vengpack");
    SpriteRender render = MakeRenderer(Context, assets, /*bloom=*/false);
    const Unique<Scene> scene = Scene::Create(Types);

    // An additive, glowing six-way sprite, so the wall behind it shows through unweighted and the
    // sprite's own contribution is the pixel minus the wall's.
    const FlipbookEmission emission{
        .Ramp = {FlipbookEmissionKey{.T = 0.5f, .Color = vec3(0.5f, 0.0f, 0.0f)}},
        .Ignition = 0.0f};
    const Entity sprite = AddSprite(*scene, SixWayFlipbook(Context, 1.0f, emission),
                                    SpriteBlend::Additive, vec3(1.0f), vec3(0.0f));
    constexpr f32 Soft = 1.0f;
    scene->Get<FlipbookSprite>(sprite).SoftDistance = Soft;
    const f32 alone = CenterRgb(render.Render(Context, *scene)).x;

    // The wall's front face at gaps of 0.1, 0.5 and 2 soft distances behind the sprite.
    constexpr f32 Edge = 4.0f;
    std::array<f32, 3> contribution{};
    const std::array<f32, 3> gaps{0.1f, 0.5f, 2.0f};
    for (usize i = 0; i < gaps.size(); ++i)
    {
        const Entity wall =
            pack.AddCube(Context, *scene, Edge, vec3(0.0f, 0.0f, gaps[i] * Soft + Edge * 0.5f));
        const f32 both = CenterRgb(render.Render(Context, *scene)).x;
        scene->Get<FlipbookSprite>(sprite).Opacity = 0.0f;
        const f32 wallOnly = CenterRgb(render.Render(Context, *scene)).x;
        scene->Get<FlipbookSprite>(sprite).Opacity = 1.0f;
        scene->DestroyEntity(wall);
        contribution[i] = both - wallOnly;
    }
    CHECK(contribution[0] < contribution[1]);
    CHECK(contribution[1] < contribution[2]);
    // Two soft distances back, nothing fades, so the reading is the sprite's over nothing; the
    // fades nearer in show the frame recorded the depth copy for the sprite alone. The tolerance
    // is two half-float steps at the sum's magnitude.
    CHECK(contribution[2] == doctest::Approx(alone).epsilon(2e-3));
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "sprite pass: under TAA a sprite shows on the frame it appears")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    SpriteRender render{
        .Renderer = SceneRenderer::Create({
            .Context = Context,
            .Assets = assets,
            .OutputFormat = Context.GetOutputFormat(),
            .Extent = Extent,
            .Settings = {.Bloom = false,
                         .AntiAliasing = AntiAliasingMode::TAA,
                         .Shadows = false,
                         .AO = false},
        }),
    };
    render.Camera.SetPerspective(glm::radians(50.0f), 1.0f, 0.1f, 100.0f);
    render.Camera.SetView(vec3(0.0f, 0.0f, -5.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));
    const Unique<Scene> scene = Scene::Create(Types);

    // A settled, valid history of the empty scene.
    for (int frame = 0; frame < 4; ++frame)
    {
        CHECK(glm::length(CenterRgb(render.Render(Context, *scene))) < 0.02f);
    }

    // Blended into a black history at the resolve's ordinary share, the sprite would read about a
    // tenth of its tint; the reactive mask lets it take nearly all of this frame.
    AddSprite(*scene, WhiteFlipbook(Context, FlipbookBlend::Alpha), SpriteBlend::Alpha, vec3(0.5f),
              vec3(0.0f));
    const vec3 center = CenterRgb(render.Render(Context, *scene));
    CHECK(center.x > 0.4f);
    CHECK(center.x < 0.55f);
}
