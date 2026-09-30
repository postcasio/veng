// SpriteScenePass on a real device: a FlipbookSprite over a black, empty scene composites the
// flipbook's frame into the lit scene color. A white premultiplied atlas makes the expected pixel the
// tint itself, so an alpha sprite shows its tint and two overlapping additive sprites show their sum;
// a sprite-less scene carries no pass and renders black, and a sprite too dim for the bloom threshold
// still lights the margin around it, which only its bloom-mask write can do.

#include <array>
#include <vector>

#include <glm/geometric.hpp>
#include <glm/gtc/packing.hpp>

#include <doctest/doctest.h>

#include <Veng/Asset/AssetBuild.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Flipbook.h>
#include <Veng/Asset/Texture.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include <gpu/fixture.h>

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

    SpriteRender MakeRenderer(Context& context, AssetManager& assets, const bool bloom)
    {
        SpriteRender render{
            .Renderer = SceneRenderer::Create({
                .Context = context,
                .Assets = assets,
                .OutputFormat = context.GetOutputFormat(),
                .Extent = Extent,
                .Settings = {.Bloom = bloom, .Shadows = false, .AO = false},
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
