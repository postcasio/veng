// Component-driven post-process effect cases (cooker + GPU gated). A PostProcessEffect scene
// component runs a fullscreen PostProcess material over the finished scene color and the scene depth
// in the HDR tail before bloom. These prove, end to end through the SceneRenderer:
//
//  - a scale-by-half effect darkens the frame, so the color input reaches the material AND every
//    downstream pre-bloom consumer (here the bloom-off tonemap) reads the effect's output rather
//    than the pre-effect scene color; a second effect chained after it darkens again (Order +
//    ping-pong);
//  - no effect (or the capture gate cleared) leaves the frame unchanged (inert / captures drop);
//  - a depth-sampling effect keeps a near cube bright and the far background dark, and reads the
//    same depth at RenderScale < 1 with a temporal mode active as at scale 1.0 — proving the depth
//    sub-rect remap co-samples color and depth;
//  - two renderers of different render scales sharing one effect material in one frame each read
//    their own view's scale map, which the per-view instance the pass draws through is what buys —
//    the material arena rings by frame-in-flight, not by view;
//  - a write to the shared instance after that per-view instance exists still reaches the draw.

#include <array>
#include <cmath>
#include <filesystem>

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

#include <glm/gtc/packing.hpp>

using namespace Veng;
using namespace Veng::Renderer;

#if defined(GPU_GBUFFER_FIXTURE_DIR) && defined(GPU_POSTPROCESS_FIXTURE_DIR)

namespace
{
    vec3 DecodeTexel(const vector<u8>& rgba16f, u32 width, u32 x, u32 y)
    {
        const auto* halves = reinterpret_cast<const u16*>(rgba16f.data());
        const usize base = (static_cast<usize>(y) * width + x) * 4;
        return vec3(glm::unpackHalf1x16(halves[base + 0]), glm::unpackHalf1x16(halves[base + 1]),
                    glm::unpackHalf1x16(halves[base + 2]));
    }

    // The default-instance ids the fixtures declare (loaded, not the material ids).
    constexpr AssetId BrickMaterial{0x895443};
    constexpr AssetId ScaleEffect{0x895640};
    constexpr AssetId ScaleEffectB{0x895641};
    constexpr AssetId DepthBandEffect{0x895642};
    constexpr AssetId ViewScaleEffect{0x3E3058326ECC6188ULL};
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "post-process effect: a scale effect darkens the frame and chains, "
                  "and the capture gate drops it")
{
    RegisterBuiltinTypes(Types);

    const path gbufferDir = path(GPU_GBUFFER_FIXTURE_DIR);
    const path postDir = path(GPU_POSTPROCESS_FIXTURE_DIR);
    const path gbufferArchive = Veng::TestSupport::TempDir() / "veng_ppe_gbuffer.vengpack";
    const path postArchive = Veng::TestSupport::TempDir() / "veng_ppe_post.vengpack";

    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    REQUIRE(cooker
                .CookPack(gbufferDir / "gbuffer_pack.json", gbufferArchive, {}, nullptr, nullptr,
                          nullptr, nullptr, {}, path(VENG_CORE_SHADER_DIR))
                .has_value());
    REQUIRE(cooker
                .CookPack(postDir / "post_effect_pack.json", postArchive, {}, nullptr, nullptr,
                          nullptr, nullptr, {}, path(VENG_CORE_SHADER_DIR))
                .has_value());

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(gbufferArchive).has_value());
    REQUIRE(assets.Mount(postArchive).has_value());

    const AssetResult<AssetHandle<MaterialInstance>> brick =
        assets.LoadSync<MaterialInstance>(BrickMaterial);
    REQUIRE(brick.has_value());
    const AssetResult<AssetHandle<MaterialInstance>> scale =
        assets.LoadSync<MaterialInstance>(ScaleEffect);
    REQUIRE(scale.has_value());
    REQUIRE(scale->Get()->GetDomain() == MaterialDomain::PostProcess);
    const AssetResult<AssetHandle<MaterialInstance>> scaleB =
        assets.LoadSync<MaterialInstance>(ScaleEffectB);
    REQUIRE(scaleB.has_value());

    constexpr uvec2 extent{128, 128};

    const Ref<Mesh> cube =
        Mesh::BuildSync(Context, Primitives::Cube(1.4f, brick.value()), "PPE Cube");

    const Unique<Scene> scene = Scene::Create(Types);
    const Entity cubeEntity = scene->CreateEntity();
    scene->Add<Transform>(cubeEntity);
    scene->Add<MeshRenderer>(cubeEntity).Mesh = assets.Adopt(cube);

    const Entity lightEntity = scene->CreateEntity();
    scene->Add<Light>(lightEntity) = Light{
        .Direction = vec3(0.0f, 0.0f, -1.0f),
        .Color = vec3(1.0f, 1.0f, 1.0f),
        .Intensity = 1.0f / LuminousAnchor,
    };

    CameraView camera;
    camera.SetPerspective(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
    camera.SetView(vec3(0.0f, 0.0f, 3.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));

    // Bloom off, so the tonemap reads the effect chain's output directly — the bloom-off tonemap
    // re-point is exactly what this exercises.
    const auto makeRenderer = [&](const bool effects)
    {
        return SceneRenderer::Create({
            .Context = Context,
            .Assets = assets,
            .OutputFormat = Context.GetOutputFormat(),
            .Extent = extent,
            .Settings = {.Mode = DebugView::Final,
                         .Bloom = false,
                         .Shadows = false,
                         .AO = false,
                         .PostProcessEffects = effects},
        });
    };

    const auto render = [&](SceneRenderer& renderer)
    {
        Context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                renderer.Execute(
                    cmd, Renderer::SceneView{.World = *scene, .Camera = camera, .Delta = 0.0f});
            });
        const vector<u8> pixels = renderer.GetOutput()->GetImage()->Download();
        return DecodeTexel(pixels, extent.x, extent.x / 2, extent.y / 2);
    };

    // Baseline: no PostProcessEffect authored — inert.
    const vec3 baseline = render(*makeRenderer(true));
    CHECK(baseline.r > 0.2f); // a lit brick face, so there is color to scale

    // One scale-by-half effect: strictly darker, so the effect ran and the tonemap read its output.
    const Entity effectEntity = scene->CreateEntity();
    scene->Add<PostProcessEffect>(effectEntity) =
        PostProcessEffect{.Material = scale.value(), .Order = 0, .Enabled = true};

    const vec3 oneEffect = render(*makeRenderer(true));
    CHECK(oneEffect.r < baseline.r);
    CHECK(oneEffect.g < baseline.g + 1e-4f);
    CHECK(oneEffect.r > 0.0f);

    // A second effect after it darkens again — Order + the ping-pong chain.
    const Entity effectEntityB = scene->CreateEntity();
    scene->Add<PostProcessEffect>(effectEntityB) =
        PostProcessEffect{.Material = scaleB.value(), .Order = 1, .Enabled = true};

    const vec3 twoEffects = render(*makeRenderer(true));
    CHECK(twoEffects.r < oneEffect.r);
    CHECK(twoEffects.r > 0.0f);

    // The capture gate: with both effects authored but PostProcessEffects cleared, the frame is the
    // baseline again — a probe drops the effects.
    const vec3 gated = render(*makeRenderer(false));
    CHECK(gated.r == doctest::Approx(baseline.r).epsilon(0.02f));

    std::filesystem::remove(gbufferArchive);
    std::filesystem::remove(postArchive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "post-process effect: depth reaches the shader and remaps under render scale")
{
    RegisterBuiltinTypes(Types);

    const path gbufferDir = path(GPU_GBUFFER_FIXTURE_DIR);
    const path postDir = path(GPU_POSTPROCESS_FIXTURE_DIR);
    const path gbufferArchive = Veng::TestSupport::TempDir() / "veng_ppe_depth_gbuffer.vengpack";
    const path postArchive = Veng::TestSupport::TempDir() / "veng_ppe_depth_post.vengpack";

    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    REQUIRE(cooker
                .CookPack(gbufferDir / "gbuffer_pack.json", gbufferArchive, {}, nullptr, nullptr,
                          nullptr, nullptr, {}, path(VENG_CORE_SHADER_DIR))
                .has_value());
    REQUIRE(cooker
                .CookPack(postDir / "post_effect_pack.json", postArchive, {}, nullptr, nullptr,
                          nullptr, nullptr, {}, path(VENG_CORE_SHADER_DIR))
                .has_value());

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(gbufferArchive).has_value());
    REQUIRE(assets.Mount(postArchive).has_value());

    const AssetResult<AssetHandle<MaterialInstance>> brick =
        assets.LoadSync<MaterialInstance>(BrickMaterial);
    REQUIRE(brick.has_value());
    const AssetResult<AssetHandle<MaterialInstance>> depthBand =
        assets.LoadSync<MaterialInstance>(DepthBandEffect);
    REQUIRE(depthBand.has_value());

    constexpr uvec2 extent{128, 128};

    // A cube filling the center; the surround is the cleared far background (reverse-Z depth 0).
    const Ref<Mesh> cube =
        Mesh::BuildSync(Context, Primitives::Cube(1.2f, brick.value()), "PPE Depth Cube");

    const Unique<Scene> scene = Scene::Create(Types);
    const Entity cubeEntity = scene->CreateEntity();
    scene->Add<Transform>(cubeEntity);
    scene->Add<MeshRenderer>(cubeEntity).Mesh = assets.Adopt(cube);
    const Entity effectEntity = scene->CreateEntity();
    scene->Add<PostProcessEffect>(effectEntity) =
        PostProcessEffect{.Material = depthBand.value(), .Order = 0, .Enabled = true};

    CameraView camera;
    camera.SetPerspective(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
    camera.SetView(vec3(0.0f, 0.0f, 3.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));

    // A temporal mode active, so the finished color is the full allocation while depth stays in the
    // render sub-rect. Occlusion off so the render scale is honoured (the GPU hi-Z test would force
    // full resolution otherwise).
    const Unique<SceneRenderer> renderer = SceneRenderer::Create({
        .Context = Context,
        .Assets = assets,
        .OutputFormat = Context.GetOutputFormat(),
        .Extent = extent,
        .Settings = {.Mode = DebugView::Final,
                     .Bloom = false,
                     .AntiAliasing = AntiAliasingMode::TAA,
                     .Shadows = false,
                     .AO = false,
                     .Occlusion = false},
    });

    const auto render = [&](const f32 renderScale)
    {
        vector<u8> pixels;
        // Two frames so the temporal history is seeded before the readback.
        for (int i = 0; i < 2; ++i)
        {
            Context.ImmediateCommands(
                [&](CommandBuffer& cmd)
                {
                    renderer->Execute(cmd, Renderer::SceneView{.World = *scene,
                                                               .Camera = camera,
                                                               .Delta = 0.016f,
                                                               .RenderScale = renderScale});
                });
        }
        pixels = renderer->GetOutput()->GetImage()->Download();
        return pixels;
    };

    // The near/far mask (red) is bright over the cube and black over the cleared far background.
    // At native scale the depth read is the identity map.
    const vector<u8> full = render(1.0f);
    const f32 fullCenter = DecodeTexel(full, extent.x, extent.x / 2, extent.y / 2).r;
    const f32 fullCorner = DecodeTexel(full, extent.x, 3, 3).r;
    CHECK(fullCenter > 0.3f);
    CHECK(fullCorner < 0.1f);

    // At render scale 0.6 with the temporal resolve active the finished color is the full allocation
    // while depth stays in the render sub-rect, so a shared UV would read depth from the wrong
    // region and the cube's mask would fall off the center. The sub-rect remap co-samples them, so
    // the center still reads the cube and the reading matches the native scale.
    const vector<u8> scaled = render(0.6f);
    const f32 scaledCenter = DecodeTexel(scaled, extent.x, extent.x / 2, extent.y / 2).r;
    const f32 scaledCorner = DecodeTexel(scaled, extent.x, 3, 3).r;
    CHECK(scaledCenter > 0.3f);
    CHECK(scaledCorner < 0.1f);
    CHECK(scaledCenter == doctest::Approx(fullCenter).epsilon(0.05f));

    std::filesystem::remove(gbufferArchive);
    std::filesystem::remove(postArchive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "post-process effect: two renderers in one frame each read their own view's "
                  "scale map")
{
    // A material's parameter block rings by frame-in-flight and not by view, so the scale maps the
    // pass derives from the recording view cannot be written into the instance the scene resolves —
    // two renderers presenting one world resolve the same handle, and the second to record would
    // decide the map for the first's draws too. The pass draws through a per-view instance instead;
    // this is the guard on that.
    RegisterBuiltinTypes(Types);

    const path postDir = path(GPU_POSTPROCESS_FIXTURE_DIR);
    const path postArchive = Veng::TestSupport::TempDir() / "veng_ppe_two_view.vengpack";

    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    REQUIRE(cooker
                .CookPack(postDir / "post_effect_pack.json", postArchive, {}, nullptr, nullptr,
                          nullptr, nullptr, {}, path(VENG_CORE_SHADER_DIR))
                .has_value());

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(postArchive).has_value());

    const AssetResult<AssetHandle<MaterialInstance>> effect =
        assets.LoadSync<MaterialInstance>(ViewScaleEffect);
    REQUIRE(effect.has_value());

    // The effect writes what its block holds rather than what it samples, so the scene needs only
    // the component that runs it.
    const Unique<Scene> scene = Scene::Create(Types);
    const Entity effectEntity = scene->CreateEntity();
    scene->Add<PostProcessEffect>(effectEntity) =
        PostProcessEffect{.Material = effect.value(), .Order = 0, .Enabled = true};

    CameraView camera;
    camera.SetPerspective(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
    camera.SetView(vec3(0.0f, 0.0f, 3.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));

    constexpr uvec2 extent{128, 128};

    // Occlusion off so the render scale is honoured (the GPU hi-Z test forces full resolution),
    // bloom off so the tonemap reads the effect's output directly.
    const auto makeRenderer = [&]
    {
        return SceneRenderer::Create({
            .Context = Context,
            .Assets = assets,
            .OutputFormat = Context.GetOutputFormat(),
            .Extent = extent,
            .Settings = {.Mode = DebugView::Final,
                         .Bloom = false,
                         .Shadows = false,
                         .AO = false,
                         .Occlusion = false},
        });
    };
    const Unique<SceneRenderer> full = makeRenderer();
    const Unique<SceneRenderer> half = makeRenderer();

    const auto view = [&](const f32 renderScale)
    {
        return Renderer::SceneView{
            .World = *scene, .Camera = camera, .Delta = 0.0f, .RenderScale = renderScale};
    };
    const auto center = [&](const SceneRenderer& renderer)
    {
        const vector<u8> pixels = renderer.GetOutput()->GetImage()->Download();
        return DecodeTexel(pixels, extent.x, extent.x / 2, extent.y / 2);
    };

    // Two frames per configuration: the first frame a scale is seen wires the post-resolve
    // promotion and recompiles the pass set, so the second is the settled one.
    const auto renderTwice = [&](auto&& body)
    {
        for (int i = 0; i < 2; ++i)
        {
            Context.ImmediateCommands([&](CommandBuffer& cmd) { body(cmd); });
        }
    };

    renderTwice([&](CommandBuffer& cmd) { full->Execute(cmd, view(1.0f)); });
    const vec3 fullAlone = center(*full);
    renderTwice([&](CommandBuffer& cmd) { half->Execute(cmd, view(0.5f)); });
    const vec3 halfAlone = center(*half);

    // The two views must genuinely disagree about the depth map, or sharing it would prove nothing.
    REQUIRE(fullAlone.r > halfAlone.r + 0.05f);

    // Both in one frame, the full-scale renderer recorded first: its draws read the block at submit,
    // after the half-scale renderer's Execute has written to it.
    renderTwice(
        [&](CommandBuffer& cmd)
        {
            full->Execute(cmd, view(1.0f));
            half->Execute(cmd, view(0.5f));
        });

    CHECK(center(*full).r == doctest::Approx(fullAlone.r).epsilon(0.02f));
    CHECK(center(*half).r == doctest::Approx(halfAlone.r).epsilon(0.02f));

    std::filesystem::remove(postArchive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "post-process effect: a write to the resolved material still reaches the draw")
{
    // The per-view instance the pass draws through is built over the resolved material's parent, so
    // it carries none of the resolved instance's own values by construction. It mirrors the resolved
    // instance's block instead, and re-mirrors whenever that block changes — which is what keeps
    // tuning an effect's params a live edit rather than a no-op.
    RegisterBuiltinTypes(Types);

    const path postDir = path(GPU_POSTPROCESS_FIXTURE_DIR);
    const path postArchive = Veng::TestSupport::TempDir() / "veng_ppe_live_write.vengpack";

    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    REQUIRE(cooker
                .CookPack(postDir / "post_effect_pack.json", postArchive, {}, nullptr, nullptr,
                          nullptr, nullptr, {}, path(VENG_CORE_SHADER_DIR))
                .has_value());

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(postArchive).has_value());

    const AssetResult<AssetHandle<MaterialInstance>> effect =
        assets.LoadSync<MaterialInstance>(ViewScaleEffect);
    REQUIRE(effect.has_value());

    const Unique<Scene> scene = Scene::Create(Types);
    const Entity effectEntity = scene->CreateEntity();
    scene->Add<PostProcessEffect>(effectEntity) =
        PostProcessEffect{.Material = effect.value(), .Order = 0, .Enabled = true};

    CameraView camera;
    camera.SetPerspective(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
    camera.SetView(vec3(0.0f, 0.0f, 3.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));

    constexpr uvec2 extent{128, 128};
    const Unique<SceneRenderer> renderer = SceneRenderer::Create({
        .Context = Context,
        .Assets = assets,
        .OutputFormat = Context.GetOutputFormat(),
        .Extent = extent,
        .Settings = {.Mode = DebugView::Final,
                     .Bloom = false,
                     .Shadows = false,
                     .AO = false,
                     .Occlusion = false},
    });

    const auto render = [&]
    {
        Context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                renderer->Execute(
                    cmd, Renderer::SceneView{.World = *scene, .Camera = camera, .Delta = 0.0f});
            });
        const vector<u8> pixels = renderer->GetOutput()->GetImage()->Download();
        return DecodeTexel(pixels, extent.x, extent.x / 2, extent.y / 2);
    };

    // The cooked default leaves the green channel at zero, and the first frame is what builds the
    // pass's per-view instance.
    const vec3 before = render();
    CHECK(before.g < 0.05f);

    // Written on the instance the scene resolved, not on the one the pass draws through.
    effect->Get()->SetParam("Tint", 0.5f);
    const vec3 after = render();
    CHECK(after.g > before.g + 0.1f);

    std::filesystem::remove(postArchive);
}

#endif // GPU_GBUFFER_FIXTURE_DIR && GPU_POSTPROCESS_FIXTURE_DIR
