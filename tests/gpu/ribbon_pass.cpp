// RibbonScenePass on a real device: a Ribbon across a black, empty scene composites its colour into
// the lit scene color along its centre line, an alpha ribbon over black reads as its colour and
// additive ones sum, a trail drawn behind a moved entity lights the path it took, a ribbon-less
// scene carries no pass and renders black, and a ribbon too dim for the bloom threshold still lights
// the margin around it, which only its bloom-mask write can do — at a reduced render scale too, where
// one pass carries the colour and the mask across to the post-resolve allocation.

#include <vector>

#include <glm/geometric.hpp>
#include <glm/gtc/packing.hpp>

#include <doctest/doctest.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/RibbonSystem.h>
#include <Veng/Scene/Scene.h>

#include <gpu/fixture.h>

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr uvec2 Extent{64, 64};

    vec3 RgbAt(const std::vector<u8>& rgba16f, const u32 x, const u32 y)
    {
        const auto* halves = reinterpret_cast<const u16*>(rgba16f.data());
        const usize base = (static_cast<usize>(y) * Extent.x + x) * 4;
        return vec3(glm::unpackHalf1x16(halves[base + 0]), glm::unpackHalf1x16(halves[base + 1]),
                    glm::unpackHalf1x16(halves[base + 2]));
    }

    struct RibbonRender
    {
        Unique<SceneRenderer> Renderer;
        CameraView Camera;

        std::vector<u8> Render(Context& context, const Scene& scene,
                               const f32 bloomThreshold = 1.0f, const f32 renderScale = 1.0f)
        {
            context.ImmediateCommands(
                [&](CommandBuffer& cmd)
                {
                    Renderer->Execute(cmd,
                                      Veng::Renderer::SceneView{.World = scene,
                                                                .Camera = Camera,
                                                                .Delta = 0.0f,
                                                                .RenderScale = renderScale,
                                                                .Exposure = 1.0f,
                                                                .Tonemapper = Tonemapper::None,
                                                                .BloomThreshold = bloomThreshold});
                });
            return Renderer->GetOutput()->GetImage()->Download();
        }
    };

    RibbonRender MakeRenderer(Context& context, AssetManager& assets, const bool bloom)
    {
        RibbonRender render{
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

    // A horizontal ribbon through the view's centre, two world units wide, so the centre pixel sits
    // on its centre line where the cross-section is full.
    Entity AddRibbon(Scene& scene, const vec3 color, const bool additive, const f32 z = 0.0f)
    {
        const Entity entity = scene.CreateEntity();
        scene.Add<Ribbon>(entity, Ribbon{.From = vec3(-3.0f, 0.0f, z),
                                         .To = vec3(3.0f, 0.0f, z),
                                         .WidthFrom = 2.0f,
                                         .WidthTo = 2.0f,
                                         .ColorFrom = color,
                                         .ColorTo = color,
                                         .Additive = additive});
        return entity;
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "ribbon pass: a ribbon shows its colour, additive ones add, and none is black")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    RibbonRender render = MakeRenderer(Context, assets, /*bloom=*/false);
    const Unique<Scene> scene = Scene::Create(Types);
    const u32 mid = Extent.x / 2;

    CHECK(glm::length(RgbAt(render.Render(Context, *scene), mid, mid)) < 0.02f);

    const Entity alpha = AddRibbon(*scene, vec3(0.5f, 0.25f, 0.125f), /*additive=*/false);
    {
        const vec3 center = RgbAt(render.Render(Context, *scene), mid, mid);
        CHECK(center.x == doctest::Approx(0.5f).epsilon(0.03));
        CHECK(center.y == doctest::Approx(0.25f).epsilon(0.03));
        CHECK(center.z == doctest::Approx(0.125f).epsilon(0.03));
    }

    scene->DestroyEntity(alpha);
    AddRibbon(*scene, vec3(0.25f), /*additive=*/true, 0.5f);
    AddRibbon(*scene, vec3(0.5f), /*additive=*/true, -0.5f);
    {
        const vec3 center = RgbAt(render.Render(Context, *scene), mid, mid);
        CHECK(center.x == doctest::Approx(0.75f).epsilon(0.03));
        CHECK(center.z == doctest::Approx(0.75f).epsilon(0.03));
    }
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "ribbon pass: a trail lights the path its entity took, not the space beside it")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    RibbonRender render = MakeRenderer(Context, assets, /*bloom=*/false);
    const Unique<Scene> scene = Scene::Create(Types);

    // The entity sweeps left to right across the view's middle row.
    const Entity mover = scene->CreateEntity();
    scene->Add<Transform>(mover, Transform{.Position = vec3(-2.0f, 0.0f, 0.0f)});
    AttachTrail(*scene, mover,
                Trail{.Lifetime = 10.0f, .Width = 0.5f, .Color = vec3(1.0f), .MaxSamples = 64});
    for (u32 step = 0; step <= 40; ++step)
    {
        const vec3 position(-2.0f + 0.1f * static_cast<f32>(step), 0.0f, 0.0f);
        scene->Get<Transform>(mover).Position = position;
        AdvanceTrail(scene->Get<Trail>(mover), position, 1.0f / 60.0f);
    }

    const std::vector<u8> pixels = render.Render(Context, *scene);
    const u32 mid = Extent.x / 2;
    CHECK(RgbAt(pixels, mid, mid).x > 0.5f);
    CHECK(RgbAt(pixels, mid, 4).x < 0.02f);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "ribbon pass: a dim additive ribbon glows through the bloom mask")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const Unique<Scene> scene = Scene::Create(Types);

    // A pixel in the black margin well clear of the ribbon, which only bloom can light.
    const auto halo = [&]()
    {
        RibbonRender render = MakeRenderer(Context, assets, /*bloom=*/true);
        return RgbAt(render.Render(Context, *scene), Extent.x / 2, 4).x;
    };

    const f32 dark = halo();
    // Luminance 0.5, below the threshold: the bright pass alone passes none of it.
    AddRibbon(*scene, vec3(0.5f), /*additive=*/true);
    const f32 lit = halo();
    CHECK(dark < 1e-3f);
    CHECK(lit > dark + 1e-3f);
}

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "ribbon pass: at a reduced render scale one pass promotes the colour and the mask")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);
    const Unique<Scene> scene = Scene::Create(Types);
    RibbonRender render = MakeRenderer(Context, assets, /*bloom=*/true);
    constexpr f32 HalfScale = 0.5f;
    const u32 mid = Extent.x / 2;

    const f32 dark = RgbAt(render.Render(Context, *scene, 1.0f, HalfScale), mid, 4).x;
    AddRibbon(*scene, vec3(0.5f), /*additive=*/true);
    const std::vector<u8> pixels = render.Render(Context, *scene, 1.0f, HalfScale);

    // Both cross the boundary in the one paired pass.
    CHECK(render.Renderer->IsPostResolveUpscaleWired());
    CHECK(render.Renderer->DidRecordPassLastFrame("Scene And Bloom Mask Upscale"));
    CHECK_FALSE(render.Renderer->DidRecordPassLastFrame("Scene Upscale"));
    CHECK_FALSE(render.Renderer->DidRecordPassLastFrame("Bloom Mask Upscale"));
    // The colour arrived: the ribbon's centre line reads at least its own colour.
    CHECK(RgbAt(pixels, mid, mid).x > 0.45f);
    // The mask arrived: below the threshold, only the mask can light the margin.
    CHECK(dark < 1e-3f);
    CHECK(RgbAt(pixels, mid, 4).x > dark + 1e-3f);
}
