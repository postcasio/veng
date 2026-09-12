// Scene-HDR-pre-bloom GUI overlays — the GuiHdrOverlayScenePass the SceneRenderer runs after the
// post-process effects and before bloom. These drive the renderer directly with a hand-built
// Gui::DrawList conveyed on SceneView::HdrOverlays (the engine-internal channel the viewport fills),
// so they pin the renderer-side contracts without a cooked document:
//
//   - an absent overlay is inert (byte-identical to no overlay), a present one composites into the
//     scene color;
//   - the overlay is in the HDR chain before tonemap — its output scales with exposure, which a
//     post-tonemap composite could not;
//   - the load-bearing cross-plan order: with a PostProcessEffect and a SceneHdrPreBloom overlay
//     both active, the effect writes the scene color, the overlay composites over it (so it ran
//     after the effect), and the overlay blooms (so it ran before bloom) — effect → overlay → bloom.
//
// A separate case drives a real GuiOverlay component through a Viewport to prove per-component
// placement routing: a SceneHdrPreBloom component composites into the scene HDR and never joins the
// post-tonemap layer stack, while a PostTonemap component does.

#include <filesystem>

#include <doctest/doctest.h>

#include <glm/gtc/packing.hpp>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Primitives.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Gui/DrawList.h>
#include <Veng/Gui/Overlay.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/LightPacking.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>

#include <gpu/fixture.h>
#include "support/TempPath.h"

using namespace Veng;
using namespace Veng::Renderer;

namespace
{
    constexpr uvec2 Extent{128, 128};

    vec3 DecodeTexel(const vector<u8>& rgba16f, u32 width, u32 x, u32 y)
    {
        const auto* halves = reinterpret_cast<const u16*>(rgba16f.data());
        const usize base = (static_cast<usize>(y) * width + x) * 4;
        return vec3(glm::unpackHalf1x16(halves[base + 0]), glm::unpackHalf1x16(halves[base + 1]),
                    glm::unpackHalf1x16(halves[base + 2]));
    }

    CameraView FrontCamera()
    {
        CameraView camera;
        camera.SetPerspective(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
        camera.SetView(vec3(0.0f, 0.0f, 3.0f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));
        return camera;
    }

    // A screen-space overlay quad covering [min, min+size) document points, opaque unless a lower
    // alpha is asked for. DocExtent == Extent, so the pass maps it 1:1 into the scene-color region.
    Gui::DrawList OverlayQuad(vec2 min, vec2 size, vec4 color)
    {
        Gui::DrawList list;
        list.Quad(Gui::Rect{.Min = min, .Size = size}, color);
        return list;
    }

    GuiHdrOverlayView ScreenSpaceView(const Gui::DrawList& list)
    {
        return GuiHdrOverlayView{
            .DrawList = &list,
            .DocExtent = vec2(Extent),
            .WorldAnchored = false,
        };
    }
}

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "gui hdr overlay: an absent overlay is inert; a present one composites into the scene")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);

    const Unique<Scene> scene = Scene::Create(Types);
    const CameraView camera = FrontCamera();

    const Unique<SceneRenderer> renderer = SceneRenderer::Create({
        .Context = Context,
        .Assets = assets,
        .OutputFormat = Context.GetOutputFormat(),
        .Extent = Extent,
        .Settings = {.Mode = DebugView::Final, .Bloom = false, .Shadows = false, .AO = false},
    });

    const auto render = [&](std::span<const GuiHdrOverlayView> overlays)
    {
        Context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                renderer->Execute(cmd, Renderer::SceneView{.World = *scene,
                                                           .Camera = camera,
                                                           .Delta = 0.0f,
                                                           .HdrOverlays = overlays});
            });
        return renderer->GetOutput()->GetImage()->Download();
    };

    // Baseline: no overlay conveyed.
    const vector<u8> baseline = render({});

    // A centered opaque overlay quad — covers [32,96), leaving a margin of empty scene.
    const Gui::DrawList list = OverlayQuad(vec2(32.0f), vec2(64.0f), vec4(0.6f, 0.2f, 0.2f, 1.0f));
    const GuiHdrOverlayView view = ScreenSpaceView(list);
    const GuiHdrOverlayView views[] = {view};
    const vector<u8> withOverlay = render(views);

    const vec3 baseCenter = DecodeTexel(baseline, Extent.x, 64, 64);
    const vec3 overlayCenter = DecodeTexel(withOverlay, Extent.x, 64, 64);
    const vec3 baseCorner = DecodeTexel(baseline, Extent.x, 4, 4);
    const vec3 overlayCorner = DecodeTexel(withOverlay, Extent.x, 4, 4);

    // The overlay changed the covered center but left the uncovered corner untouched — it drew, and
    // is inert where it does not cover.
    CHECK(overlayCenter.r > baseCenter.r + 0.05f);
    CHECK(overlayCorner.r == doctest::Approx(baseCorner.r).epsilon(0.01f));

    // An empty conveyed span reproduces the baseline exactly — absent overlay, no extra work.
    const vector<u8> emptyAgain = render({});
    CHECK(emptyAgain == baseline);
}

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "gui hdr overlay: the overlay is in the HDR chain — its output scales with exposure")
{
    RegisterBuiltinTypes(Types);
    AssetManager assets(Context, Tasks, Types);

    const Unique<Scene> scene = Scene::Create(Types);
    const CameraView camera = FrontCamera();

    const Unique<SceneRenderer> renderer = SceneRenderer::Create({
        .Context = Context,
        .Assets = assets,
        .OutputFormat = Context.GetOutputFormat(),
        .Extent = Extent,
        .Settings = {.Mode = DebugView::Final, .Bloom = false, .Shadows = false, .AO = false},
    });

    const Gui::DrawList list = OverlayQuad(vec2(32.0f), vec2(64.0f), vec4(0.5f, 0.5f, 0.5f, 1.0f));
    const GuiHdrOverlayView view = ScreenSpaceView(list);
    const GuiHdrOverlayView views[] = {view};

    const auto renderAtExposure = [&](const f32 exposure)
    {
        Context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                renderer->Execute(cmd, Renderer::SceneView{.World = *scene,
                                                           .Camera = camera,
                                                           .Delta = 0.0f,
                                                           .Exposure = exposure,
                                                           .HdrOverlays = views});
            });
        return DecodeTexel(renderer->GetOutput()->GetImage()->Download(), Extent.x, 64, 64);
    };

    // The overlay is composited into the HDR before tonemap, so a higher exposure brightens it — a
    // post-tonemap composite would be exposure-independent.
    const vec3 low = renderAtExposure(1.0f);
    const vec3 high = renderAtExposure(4.0f);
    CHECK(high.r > low.r + 0.05f);
}

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "gui hdr overlay: effect then overlay then bloom — the load-bearing cross-plan order")
{
    RegisterBuiltinTypes(Types);

    const path gbufferDir = path(GPU_GBUFFER_FIXTURE_DIR);
    const path postDir = path(GPU_POSTPROCESS_FIXTURE_DIR);
    const path gbufferArchive =
        Veng::TestSupport::TempDir() / "veng_gui_hdr_overlay_gbuffer.vengpack";
    const path postArchive = Veng::TestSupport::TempDir() / "veng_gui_hdr_overlay_post.vengpack";
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

    // A lit cube (a scene with real color to darken) and the scale-by-half post effect.
    constexpr AssetId BrickMaterial{0x895443};
    constexpr AssetId ScaleEffect{0x895640};
    const AssetResult<AssetHandle<MaterialInstance>> brick =
        assets.LoadSync<MaterialInstance>(BrickMaterial);
    REQUIRE(brick.has_value());
    const AssetResult<AssetHandle<MaterialInstance>> scale =
        assets.LoadSync<MaterialInstance>(ScaleEffect);
    REQUIRE(scale.has_value());
    REQUIRE(scale->Get()->GetDomain() == MaterialDomain::PostProcess);

    const Ref<Mesh> cube =
        Mesh::BuildSync(Context, Primitives::Cube(1.6f, brick.value()), "Overlay Cube");

    const Unique<Scene> scene = Scene::Create(Types);
    const Entity cubeEntity = scene->CreateEntity();
    scene->Add<Transform>(cubeEntity);
    scene->Add<MeshRenderer>(cubeEntity).Mesh = assets.Adopt(cube);
    const Entity lightEntity = scene->CreateEntity();
    scene->Add<Light>(lightEntity) = Light{.Direction = vec3(0.0f, 0.0f, -1.0f),
                                           .Color = vec3(1.0f, 1.0f, 1.0f),
                                           .Intensity = 1.0f / LuminousAnchor};

    const Entity effectEntity = scene->CreateEntity();
    const CameraView camera = FrontCamera();

    // A fresh renderer per configuration (the plan-00 pattern): each first Execute resolves the
    // effect and overlay sets from a clean state, avoiding a reused renderer's toggle churn.
    const auto makeRenderer = [&](const bool bloom)
    {
        return SceneRenderer::Create({
            .Context = Context,
            .Assets = assets,
            .OutputFormat = Context.GetOutputFormat(),
            .Extent = Extent,
            .Settings = {.Mode = DebugView::Final, .Bloom = bloom, .Shadows = false, .AO = false},
        });
    };
    const auto renderOnce =
        [&](SceneRenderer& renderer, std::span<const GuiHdrOverlayView> overlays)
    {
        Context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                renderer.Execute(cmd, Renderer::SceneView{.World = *scene,
                                                          .Camera = camera,
                                                          .Delta = 0.0f,
                                                          .BloomThreshold = 1.0f,
                                                          .HdrOverlays = overlays});
            });
        return renderer.GetOutput()->GetImage()->Download();
    };
    const auto setEffect = [&](const bool enabled)
    {
        if (scene->Has<PostProcessEffect>(effectEntity))
        {
            (void)scene->Remove<PostProcessEffect>(effectEntity);
        }
        if (enabled)
        {
            scene->Add<PostProcessEffect>(effectEntity) =
                PostProcessEffect{.Material = scale.value(), .Order = 0, .Enabled = true};
        }
    };

    // A bright opaque overlay over a small centered patch [48,80), on the cube face — its tonemapped
    // value clears the lit cube and it blooms.
    const Gui::DrawList list = OverlayQuad(vec2(48.0f), vec2(32.0f), vec4(8.0f, 8.0f, 8.0f, 1.0f));
    const GuiHdrOverlayView view = ScreenSpaceView(list);
    const GuiHdrOverlayView views[] = {view};

    constexpr uvec2 OverlayPix{64, 64}; // inside the overlay patch, over the cube
    constexpr uvec2 ScenePix{88, 64};   // lit cube, outside the patch

    // Effect off: the plain lit cube, and the opaque overlay over it.
    setEffect(false);
    const Unique<SceneRenderer> plainRenderer = makeRenderer(false);
    const f32 scenePlain =
        DecodeTexel(renderOnce(*plainRenderer, {}), Extent.x, ScenePix.x, ScenePix.y).r;
    const Unique<SceneRenderer> overlayRenderer = makeRenderer(false);
    const f32 overlayNoEffect =
        DecodeTexel(renderOnce(*overlayRenderer, views), Extent.x, OverlayPix.x, OverlayPix.y).r;

    // Effect on (scale-by-half): the darkened cube, then the overlay over the darkened cube, each
    // rendered with bloom off and bloom on so the overlay's bloom contribution can be isolated.
    setEffect(true);
    const Unique<SceneRenderer> effectRenderer = makeRenderer(false);
    const vector<u8> effectNoOverlay = renderOnce(*effectRenderer, {});
    const f32 sceneEffect = DecodeTexel(effectNoOverlay, Extent.x, ScenePix.x, ScenePix.y).r;
    const Unique<SceneRenderer> effectOverlayRenderer = makeRenderer(false);
    const vector<u8> effectOverlay = renderOnce(*effectOverlayRenderer, views);
    const f32 overlayEffect = DecodeTexel(effectOverlay, Extent.x, OverlayPix.x, OverlayPix.y).r;

    const Unique<SceneRenderer> effectBloomRenderer = makeRenderer(true);
    const vector<u8> effectNoOverlayBloom = renderOnce(*effectBloomRenderer, {});
    const Unique<SceneRenderer> effectOverlayBloomRenderer = makeRenderer(true);
    const vector<u8> effectOverlayBloom = renderOnce(*effectOverlayBloomRenderer, views);

    // The effect ran: the scale-by-half darkens the visible cube where the overlay does not cover.
    CHECK(scenePlain > 0.1f);
    CHECK(sceneEffect < scenePlain - 0.02f);

    // effect → overlay: the opaque overlay pixel is the same with or without the effect — the effect
    // darkened only the scene the overlay covers, so the overlay composited after the effect (had it
    // run before, the scale would have halved the overlay too), and it clears the lit cube.
    CHECK(overlayEffect == doctest::Approx(overlayNoEffect).epsilon(0.02f));
    CHECK(overlayEffect > scenePlain + 0.1f);

    // overlay survives bloom: with an effect active and bloom on, the bright opaque overlay patch is
    // still present in the final image — far brighter than the same pixel with no overlay conveyed.
    // Bloom samples the effect chain's output (which the overlay composited into), not the raw HDR
    // beneath the effect; sampling the raw HDR drops the overlay from every bloom-on frame.
    const f32 overlayBloomPatch =
        DecodeTexel(effectOverlayBloom, Extent.x, OverlayPix.x, OverlayPix.y).r;
    const f32 noOverlayBloomPatch =
        DecodeTexel(effectNoOverlayBloom, Extent.x, OverlayPix.x, OverlayPix.y).r;
    CHECK(overlayBloomPatch > noOverlayBloomPatch + 0.3f);

    // overlay → bloom: just outside the patch the overlay leaves a bloom halo — energy present only
    // because the overlay was composited before the bloom read. Differencing the same pixel with and
    // without the overlay isolates the overlay's own halo from the cube's.
    constexpr uvec2 HaloPix{64, 84}; // 4 px below the [48,80) patch, over the cube
    const f32 overlayBloomHalo = DecodeTexel(effectOverlayBloom, Extent.x, HaloPix.x, HaloPix.y).r;
    const f32 noOverlayBloomHalo =
        DecodeTexel(effectNoOverlayBloom, Extent.x, HaloPix.x, HaloPix.y).r;
    CHECK(overlayBloomHalo > noOverlayBloomHalo + 0.02f);

    std::filesystem::remove(gbufferArchive);
    std::filesystem::remove(postArchive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture, "gui hdr overlay: a composite material shapes the color "
                                          "and blooms by its mask, not its brightness")
{
    RegisterBuiltinTypes(Types);

    // A PostProcess-domain composite material declaring a bloom mask (the glow-split contract): it
    // samples the overlay's document (which the renderer rendered to the intermediate), tints it, and
    // writes shaped color + a flat bloom amplitude.
    const path compositeDir = path(GPU_POSTPROCESS_FIXTURE_DIR);
    const path archive = Veng::TestSupport::TempDir() / "veng_gui_hdr_overlay_composite.vengpack";
    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    REQUIRE(cooker
                .CookPack(compositeDir / "overlay_composite_pack.json", archive, {}, nullptr,
                          nullptr, nullptr, nullptr, {}, path(VENG_CORE_SHADER_DIR))
                .has_value());

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(archive).has_value());

    // The default instance tints (2.0, 0.5, 0.5) and asks for a flat mask amplitude of 6.
    constexpr AssetId CompositeInstance{0x00000000008A0010ULL};
    const AssetResult<AssetHandle<MaterialInstance>> composite =
        assets.LoadSync<MaterialInstance>(CompositeInstance);
    REQUIRE(composite.has_value());
    REQUIRE(composite->Get()->GetDomain() == MaterialDomain::PostProcess);
    MaterialInstance* const material = composite->Get();

    // An empty scene: a black ground the mask-driven halo reads against.
    const Unique<Scene> scene = Scene::Create(Types);
    const CameraView camera = FrontCamera();

    // A centered opaque quad in a saturated, non-white-hot colour — its luminance (~0.36) is far below
    // the bloom threshold (1.0), so nothing about how bright it is drawn will bloom it.
    const Gui::DrawList list = OverlayQuad(vec2(40.0f), vec2(48.0f), vec4(0.2f, 0.35f, 0.9f, 1.0f));

    const auto render = [&](const bool bloom, const bool withMaterial, const bool withOverlay)
    {
        const Unique<SceneRenderer> renderer = SceneRenderer::Create({
            .Context = Context,
            .Assets = assets,
            .OutputFormat = Context.GetOutputFormat(),
            .Extent = Extent,
            .Settings = {.Mode = DebugView::Final, .Bloom = bloom, .Shadows = false, .AO = false},
        });
        GuiHdrOverlayView view = ScreenSpaceView(list);
        view.Material = withMaterial ? material : nullptr;
        const GuiHdrOverlayView views[] = {view};
        const std::span<const GuiHdrOverlayView> conveyed =
            withOverlay ? std::span<const GuiHdrOverlayView>(views)
                        : std::span<const GuiHdrOverlayView>();
        Context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                renderer->Execute(cmd, Renderer::SceneView{.World = *scene,
                                                           .Camera = camera,
                                                           .Delta = 0.0f,
                                                           .BloomThreshold = 1.0f,
                                                           .HdrOverlays = conveyed});
            });
        return renderer->GetOutput()->GetImage()->Download();
    };

    constexpr uvec2 CenterPix{64, 64}; // inside the quad
    constexpr uvec2 HaloPix{64, 92};   // in the black margin below the quad's [40,88) bottom edge

    // (1) The composited colour reaches the scene HDR, shaped by the material. Its tint boosts red and
    // cuts green, so the material composite reads redder and less green than the straight document a
    // direct (no-material) overlay blends — proof the material fragment ran and shaped the colour.
    const vec3 matCenter =
        DecodeTexel(render(false, true, true), Extent.x, CenterPix.x, CenterPix.y);
    const vec3 directCenter =
        DecodeTexel(render(false, false, true), Extent.x, CenterPix.x, CenterPix.y);
    CHECK(matCenter.r > directCenter.r + 0.05f);
    CHECK(matCenter.g < directCenter.g - 0.02f);
    CHECK(matCenter.r > 0.1f);

    // (2) The mask drives the bloom, decoupled from the drawn brightness. With bloom on, the material
    // overlay leaves a halo in the black margin — energy the flat mask seeded — while the SAME dim
    // element with NO material does not bloom, because its luminance is far below the threshold. That
    // is the whole point of the split: an element glows in its own colour without going white to earn
    // it. Both are differenced against the no-overlay baseline (black there).
    const f32 baselineHalo =
        DecodeTexel(render(true, false, false), Extent.x, HaloPix.x, HaloPix.y).r;
    const f32 directHalo = DecodeTexel(render(true, false, true), Extent.x, HaloPix.x, HaloPix.y).r;
    const vec3 matHalo = DecodeTexel(render(true, true, true), Extent.x, HaloPix.x, HaloPix.y);

    CHECK(baselineHalo == doctest::Approx(0.0f).epsilon(0.01f));
    // The masked overlay blooms into the margin; the unmasked one does not (it stays at the baseline).
    CHECK(matHalo.r > directHalo + 0.02f);
    CHECK(directHalo == doctest::Approx(baselineHalo).epsilon(0.01f));

    std::filesystem::remove(archive);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "gui hdr overlay: two material overlays in one frame both "
                  "composite — neither overwrites the other's geometry")
{
    RegisterBuiltinTypes(Types);

    // The same composite material both overlays name — the shared-instance case the HUD uses (its
    // head-up pane and console MFD both composite through one glow-split material each frame).
    const path compositeDir = path(GPU_POSTPROCESS_FIXTURE_DIR);
    const path archive = Veng::TestSupport::TempDir() / "veng_gui_hdr_overlay_two.vengpack";
    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    REQUIRE(cooker
                .CookPack(compositeDir / "overlay_composite_pack.json", archive, {}, nullptr,
                          nullptr, nullptr, nullptr, {}, path(VENG_CORE_SHADER_DIR))
                .has_value());

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(archive).has_value());

    constexpr AssetId CompositeInstance{0x00000000008A0010ULL};
    const AssetResult<AssetHandle<MaterialInstance>> composite =
        assets.LoadSync<MaterialInstance>(CompositeInstance);
    REQUIRE(composite.has_value());
    MaterialInstance* const material = composite->Get();

    const Unique<Scene> scene = Scene::Create(Types);
    const CameraView camera = FrontCamera();

    // Two overlays over disjoint screen regions: a top-left quad and a bottom-right one. Their
    // documents are recorded through one GuiScenePass in the same frame, so this is the case where a
    // frame-keyed geometry ring lets the second overwrite the first before either draw executes.
    const Gui::DrawList listA =
        OverlayQuad(vec2(16.0f), vec2(32.0f), vec4(0.2f, 0.35f, 0.9f, 1.0f));
    const Gui::DrawList listB =
        OverlayQuad(vec2(80.0f), vec2(32.0f), vec4(0.2f, 0.35f, 0.9f, 1.0f));

    const auto render = [&](const std::span<const GuiHdrOverlayView> overlays)
    {
        const Unique<SceneRenderer> renderer = SceneRenderer::Create({
            .Context = Context,
            .Assets = assets,
            .OutputFormat = Context.GetOutputFormat(),
            .Extent = Extent,
            .Settings = {.Mode = DebugView::Final, .Bloom = false, .Shadows = false, .AO = false},
        });
        Context.ImmediateCommands(
            [&](CommandBuffer& cmd)
            {
                renderer->Execute(cmd, Renderer::SceneView{.World = *scene,
                                                           .Camera = camera,
                                                           .Delta = 0.0f,
                                                           .BloomThreshold = 1.0f,
                                                           .HdrOverlays = overlays});
            });
        return renderer->GetOutput()->GetImage()->Download();
    };

    GuiHdrOverlayView viewA = ScreenSpaceView(listA);
    viewA.Material = material;
    GuiHdrOverlayView viewB = ScreenSpaceView(listB);
    viewB.Material = material;
    const GuiHdrOverlayView both[] = {viewA, viewB};

    constexpr uvec2 CenterA{32, 32}; // inside quad A [16,48)
    constexpr uvec2 CenterB{96, 96}; // inside quad B [80,112)
    constexpr uvec2 Corner{112, 16}; // in neither quad

    const vector<u8> pixels = render(both);
    const vec3 a = DecodeTexel(pixels, Extent.x, CenterA.x, CenterA.y);
    const vec3 b = DecodeTexel(pixels, Extent.x, CenterB.x, CenterB.y);
    const vec3 corner = DecodeTexel(pixels, Extent.x, Corner.x, Corner.y);

    // Both regions carry a material-shaped composite (its tint boosts red past 0.1) — the first
    // overlay is not blanked by the second's document overwriting the shared geometry ring, which is
    // the regression: with a frame-keyed ring the earlier draw reads the later document's vertices,
    // leaving region A black. The margin between them stays black, so each drew only its own quad.
    CHECK(a.r > 0.1f);
    CHECK(b.r > 0.1f);
    CHECK(corner.r == doctest::Approx(0.0f).epsilon(0.01f));

    // Each overlay's region matches what it shows composited alone — neither perturbs the other.
    const GuiHdrOverlayView onlyA[] = {viewA};
    const GuiHdrOverlayView onlyB[] = {viewB};
    const vec3 aAlone = DecodeTexel(render(onlyA), Extent.x, CenterA.x, CenterA.y);
    const vec3 bAlone = DecodeTexel(render(onlyB), Extent.x, CenterB.x, CenterB.y);
    CHECK(a.r == doctest::Approx(aAlone.r).epsilon(0.02f));
    CHECK(b.r == doctest::Approx(bAlone.r).epsilon(0.02f));

    std::filesystem::remove(archive);
}

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "gui hdr overlay: placement routes per component — HDR overlay skips the layer stack")
{
    RegisterBuiltinTypes(Types);

    // The shared HUD fixture document (tests/cooker/fixtures/ui_hud_pack.json).
    constexpr AssetId UIDocumentId{0xA09AA8B60AEAA8BEULL};
    const path packJson = path(GPU_COOKER_FIXTURE_DIR) / "ui_hud_pack.json";
    const path archive = Veng::TestSupport::TempDir() / "veng_gui_hdr_overlay_hud.vengpack";
    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    REQUIRE(cooker.CookPack(packJson, archive).has_value());

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(archive).has_value());

    const auto makeViewport = [&]
    {
        return Viewport::Create({
            .Context = Context,
            .Assets = assets,
            .Region = {.Offset = {0, 0}, .Extent = Extent},
            .ColorFormat = Format::RGBA16Sfloat,
            .Role = ViewportRole::Presented,
        });
    };

    // Baseline: an empty scene, no overlay.
    const Unique<Scene> baseScene = Scene::Create(Types);
    const Unique<Viewport> baseViewport = makeViewport();
    baseViewport->SetViewState({.World = baseScene.get(), .Delta = 0.016f});
    Context.ImmediateCommands([&](CommandBuffer& cmd) { baseViewport->Render(cmd); });
    const vector<u8> baseline = baseViewport->GetOutput()->GetImage()->Download();

    // A SceneHdrPreBloom overlay component: driven ahead of the render, composited into the scene HDR,
    // and never attached to the post-tonemap layer stack.
    const Unique<Scene> hdrScene = Scene::Create(Types);
    const Entity hdrEntity = hdrScene->CreateEntity();
    {
        auto& overlay = hdrScene->Add<GuiOverlay>(hdrEntity);
        overlay.Document = *assets.LoadSync<Gui::UIDocument>(UIDocumentId);
        overlay.Placement = GuiOverlayPlacement::SceneHdrPreBloom;
    }
    const Unique<Viewport> hdrViewport = makeViewport();
    hdrViewport->SetViewState({.World = hdrScene.get(), .Delta = 0.016f});
    Context.ImmediateCommands([&](CommandBuffer& cmd) { hdrViewport->Render(cmd); });
    const vector<u8> hdrOutput = hdrViewport->GetOutput()->GetImage()->Download();

    // The HDR overlay changed the rendered scene but joined no layer stack.
    CHECK(hdrOutput != baseline);
    CHECK(hdrViewport->GetAttachedDocuments().empty());

    // A PostTonemap overlay of the same document instead attaches to the layer stack (today's path).
    const Unique<Scene> ldrScene = Scene::Create(Types);
    const Entity ldrEntity = ldrScene->CreateEntity();
    {
        auto& overlay = ldrScene->Add<GuiOverlay>(ldrEntity);
        overlay.Document = *assets.LoadSync<Gui::UIDocument>(UIDocumentId);
        overlay.Placement = GuiOverlayPlacement::PostTonemap;
    }
    const Unique<Viewport> ldrViewport = makeViewport();
    ldrViewport->SetViewState({.World = ldrScene.get(), .Delta = 0.016f});
    Context.ImmediateCommands([&](CommandBuffer& cmd) { ldrViewport->Render(cmd); });
    CHECK(ldrViewport->GetAttachedDocuments().size() == 1);

    std::filesystem::remove(archive);
}

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "gui hdr overlay: a hidden overlay draws nothing and attaches nothing, both placements")
{
    RegisterBuiltinTypes(Types);

    constexpr AssetId UIDocumentId{0xA09AA8B60AEAA8BEULL};
    const path packJson = path(GPU_COOKER_FIXTURE_DIR) / "ui_hud_pack.json";
    const path archive = Veng::TestSupport::TempDir() / "veng_gui_hdr_overlay_hidden.vengpack";
    Cook::Cooker cooker;
    Cook::RegisterBuiltinImporters(cooker);
    REQUIRE(cooker.CookPack(packJson, archive).has_value());

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(archive).has_value());

    const auto makeViewport = [&]
    {
        return Viewport::Create({
            .Context = Context,
            .Assets = assets,
            .Region = {.Offset = {0, 0}, .Extent = Extent},
            .ColorFormat = Format::RGBA16Sfloat,
            .Role = ViewportRole::Presented,
        });
    };

    // Baseline: an empty scene, no overlay.
    const Unique<Scene> baseScene = Scene::Create(Types);
    const Unique<Viewport> baseViewport = makeViewport();
    baseViewport->SetViewState({.World = baseScene.get(), .Delta = 0.016f});
    Context.ImmediateCommands([&](CommandBuffer& cmd) { baseViewport->Render(cmd); });
    const vector<u8> baseline = baseViewport->GetOutput()->GetImage()->Download();

    // A SceneHdrPreBloom overlay with Visible = false builds nothing into the pre-bloom pass, so the
    // rendered scene is byte-identical to the baseline — the overlay is suppressed, not merely faint.
    const Unique<Scene> hdrScene = Scene::Create(Types);
    const Entity hdrEntity = hdrScene->CreateEntity();
    {
        auto& overlay = hdrScene->Add<GuiOverlay>(hdrEntity);
        overlay.Document = *assets.LoadSync<Gui::UIDocument>(UIDocumentId);
        overlay.Placement = GuiOverlayPlacement::SceneHdrPreBloom;
        overlay.Visible = false;
    }
    const Unique<Viewport> hdrViewport = makeViewport();
    hdrViewport->SetViewState({.World = hdrScene.get(), .Delta = 0.016f});
    Context.ImmediateCommands([&](CommandBuffer& cmd) { hdrViewport->Render(cmd); });
    CHECK(hdrViewport->GetOutput()->GetImage()->Download() == baseline);

    // Restoring Visible re-presents the overlay with no reload — it now draws, so the output differs
    // from the baseline.
    hdrScene->Get<GuiOverlay>(hdrEntity).Visible = true;
    Context.ImmediateCommands([&](CommandBuffer& cmd) { hdrViewport->Render(cmd); });
    CHECK(hdrViewport->GetOutput()->GetImage()->Download() != baseline);

    // A PostTonemap overlay with Visible = false is skipped and detached, so nothing joins the layer
    // stack; restoring the flag attaches it.
    const Unique<Scene> ldrScene = Scene::Create(Types);
    const Entity ldrEntity = ldrScene->CreateEntity();
    {
        auto& overlay = ldrScene->Add<GuiOverlay>(ldrEntity);
        overlay.Document = *assets.LoadSync<Gui::UIDocument>(UIDocumentId);
        overlay.Placement = GuiOverlayPlacement::PostTonemap;
        overlay.Visible = false;
    }
    const Unique<Viewport> ldrViewport = makeViewport();
    ldrViewport->SetViewState({.World = ldrScene.get(), .Delta = 0.016f});
    Context.ImmediateCommands([&](CommandBuffer& cmd) { ldrViewport->Render(cmd); });
    CHECK(ldrViewport->GetAttachedDocuments().empty());

    ldrScene->Get<GuiOverlay>(ldrEntity).Visible = true;
    Context.ImmediateCommands([&](CommandBuffer& cmd) { ldrViewport->Render(cmd); });
    CHECK(ldrViewport->GetAttachedDocuments().size() == 1);

    std::filesystem::remove(archive);
}
