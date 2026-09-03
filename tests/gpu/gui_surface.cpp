// GuiSurface end-to-end: a live Gui::Document mapped onto a world quad in a scene, driven into its
// persistent HDR target ahead of the scene render and turned into scene light through one of the two
// delivered material domains. The panel's bright document background is authored above 1.0, so it
// blooms through the scene's own bloom (no dedicated GUI bloom pass). The cases assert:
//
//  - the panel's HDR target holds the document's >1.0 radiance (the value the surface material
//    samples), surviving the half-float round-trip the material read consumes;
//  - the panel content reaches the lit scene color and the glow is visible in the rendered output;
//  - Translucent (default) is self-radiant and see-through — a transparent document region shows the
//    scene behind the panel (the panel writes no depth, so it does not occlude);
//  - OpaqueEmissive occludes — its opaque g-buffer surface writes depth, so the same transparent
//    region shows the panel's dark bezel, not the scene behind, and the EmissiveColor=(1,1,1) default
//    passes the document value through;
//  - the dirty-gate: an idle panel does not re-record its document on a frame where nothing changed.
//
// The whole render runs under the validation gate, so the producer-before-consumer handoff (the
// panel target is left shader-readable by its own barrier before the translucent pass or the
// opaque g-buffer surface samples it) is validation-clean or the gate fails.

#include <doctest/doctest.h>

#include <glm/gtc/packing.hpp>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Material.h>
#include <Veng/Audio/AudioDevice.h>
#include <Veng/Audio/AudioEngine.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Primitives.h>
#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>
#include <Veng/Gui/Document.h>
#include <Veng/Gui/Driver.h>
#include <Veng/Gui/DriverRegistry.h>
#include <Veng/Gui/Element.h>
#include <Veng/Gui/RenderTarget.h>
#include <Veng/Gui/Style.h>
#include <Veng/Gui/Surface.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/LightPacking.h>
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
    constexpr uvec2 PanelRes{128, 128};

    // The panel document's bright fill: glowing cyan whose green/blue components are 4.0, well above
    // the 1.0 an 8-bit target would clamp to and the point at which the scene's bloom engages.
    constexpr vec4 Glow{0.0f, 4.0f, 4.0f, 1.0f};

    // Cooked material ids in the gui_surface fixture pack.
    constexpr AssetId TranslucentInstance{0x2417};
    constexpr AssetId EmissiveInstance{0x2416};
    constexpr AssetId BrickInstance{0x895443};

    // Decodes one RGBA16Sfloat texel to a linear vec4.
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

    // Cooks the gui_surface fixture pack in-process; its shaders `#include "Veng/..."`, so the engine
    // core shader dir is threaded onto the Slang search path.
    path CookGuiSurfacePack()
    {
        const path fixtureDir = path(GPU_GBUFFER_FIXTURE_DIR);
        const path outArchive = Veng::TestSupport::TempDir() / "veng_gui_surface.vengpack";

        Cook::Cooker cooker;
        Cook::RegisterBuiltinImporters(cooker);
        const VoidResult cookResult =
            cooker.CookPack(fixtureDir / "gui_surface_pack.json", outArchive, {}, nullptr, nullptr,
                            nullptr, nullptr, {}, path(VENG_CORE_SHADER_DIR));
        REQUIRE(cookResult.has_value());
        return outArchive;
    }

    // A document filling the target: a transparent root centering one glowing child panel that
    // covers the middle third, so the target center is bright and its border is transparent.
    Unique<Gui::Document> BuildPanelDocument()
    {
        auto document = CreateUnique<Gui::Document>();

        Gui::Style root;
        root.Width = Gui::Length::Points(static_cast<f32>(PanelRes.x));
        root.Height = Gui::Length::Points(static_cast<f32>(PanelRes.y));
        root.Background = vec4(0.0f);
        root.JustifyContent = Gui::Justify::Center;
        root.AlignItems = Gui::Align::Center;
        document->SetStyle(document->Root(), root);

        Gui::Element& child = document->Add(document->Root(), Gui::ElementKind::Panel);
        Gui::Style childStyle;
        childStyle.Width = Gui::Length::Points(static_cast<f32>(PanelRes.x) * 0.34f);
        childStyle.Height = Gui::Length::Points(static_cast<f32>(PanelRes.y) * 0.34f);
        childStyle.Background = Glow;
        document->SetStyle(child, childStyle);

        return document;
    }

    CameraView FrontCamera()
    {
        CameraView camera;
        camera.SetPerspective(glm::radians(45.0f), 1.0f, 0.1f, 100.0f);
        camera.SetView(vec3(0.0f, 0.0f, 3.2f), vec3(0.0f), vec3(0.0f, 1.0f, 0.0f));
        return camera;
    }

    // Builds a scene: a bright brick backdrop cube filling the view, a directional light, and a panel
    // quad cube (extent 1.4) at the origin carrying the given material and a GuiSurface driving the
    // panel document. Keeps the built meshes alive in `meshes`.
    Unique<Scene> BuildPanelScene(Context& context, AssetManager& assets, TypeRegistry& types,
                                  GuiSurfaceDomain domain,
                                  const AssetHandle<MaterialInstance>& panelMaterial,
                                  const AssetHandle<MaterialInstance>& backdropMaterial,
                                  vector<Ref<Mesh>>& meshes, Entity& panelEntity)
    {
        Unique<Scene> scene = Scene::Create(types);

        // A large backdrop well behind the panel (front face at z = -2, the panel's is at z = +0.7),
        // filling the view behind the panel so a see-through region reveals it.
        const Ref<Mesh> backdrop = Mesh::BuildSync(
            context, Primitives::Cube(6.0f, backdropMaterial), "GuiSurface Backdrop");
        meshes.push_back(backdrop);
        const Entity backdropEntity = scene->CreateEntity();
        scene->Add<Transform>(backdropEntity).Position = vec3(0.0f, 0.0f, -5.0f);
        scene->Add<MeshRenderer>(backdropEntity).Mesh = assets.Adopt(backdrop);

        const Entity lightEntity = scene->CreateEntity();
        auto& light = scene->Add<Light>(lightEntity);
        light.Direction = glm::normalize(vec3(0.0f, 0.0f, -1.0f));
        light.Color = vec3(1.0f);
        // A directional's intensity is an illuminance in lux (internal radiance 2.0 at the anchor).
        light.Intensity = 2.0f / Renderer::LuminousAnchor;

        const Ref<Mesh> panel =
            Mesh::BuildSync(context, Primitives::Cube(1.4f, panelMaterial), "GuiSurface Panel");
        meshes.push_back(panel);
        panelEntity = scene->CreateEntity();
        scene->Add<Transform>(panelEntity);
        scene->Add<MeshRenderer>(panelEntity).Mesh = assets.Adopt(panel);

        auto& surface = scene->Add<GuiSurface>(panelEntity);
        surface.Resolution = PanelRes;
        surface.Domain = domain;
        surface.SetDocument(BuildPanelDocument());

        return scene;
    }

    // What the panel driver recorded on its most recent run. File-scope because the surface owns the
    // driver instance and hands no route back to it; exactly one instance runs per case.
    struct DriverTrace
    {
        int Instantiates = 0;
        int Updates = 0;
        Entity Owner;
        f32 Alpha = -1.0f;
        uvec2 RegionExtent{0};
        const AssetManager* Assets = nullptr;
        const Audio::AudioEngine* Audio = nullptr;
    };

    DriverTrace g_Trace;

    // Recolors the document's glowing child on every update, so the panel's pixels prove the driver
    // ran *before* the document's dirty-gated record rather than after it.
    struct PanelDriver final : GuiDriver
    {
        void OnInstantiate(Gui::Document&, Scene&, Entity) override { ++g_Trace.Instantiates; }

        void OnUpdate(const GuiDriverFrame& frame) override
        {
            ++g_Trace.Updates;
            g_Trace.Owner = frame.Owner;
            g_Trace.Alpha = frame.Alpha;
            g_Trace.RegionExtent = frame.View.Region.Extent;
            g_Trace.Assets = &frame.Assets;
            g_Trace.Audio = frame.Audio;

            Gui::Element& root = frame.Document.Root();
            REQUIRE(root.Children.size() == 1);
            Gui::Style style = root.Children[0]->BaseStyle;
            style.Background = vec4(Glow.g * static_cast<f32>(g_Trace.Updates), 0.0f, 0.0f, 1.0f);
            frame.Document.SetStyle(*root.Children[0], style);
        }
    };

    Unique<Viewport> MakeViewport(Context& context, AssetManager& assets)
    {
        return Viewport::Create({
            .Context = context,
            .Assets = assets,
            .Region = {.Offset = {0, 0}, .Extent = Extent},
            .ColorFormat = Format::RGBA16Sfloat,
            .Role = ViewportRole::Offscreen,
        });
    }
}

VE_GUI_DRIVER(PanelDriver, 0x9E1C0F2A73B4D586ULL, "Panel");

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "gui surface: a translucent panel is self-radiant and see-through, an opaque "
                  "emissive panel occludes — both bloom through the scene")
{
    RegisterBuiltinTypes(Types);

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(CookGuiSurfacePack()).has_value());

    const AssetResult<AssetHandle<MaterialInstance>> translucent =
        assets.LoadSync<MaterialInstance>(TranslucentInstance);
    const AssetResult<AssetHandle<MaterialInstance>> emissive =
        assets.LoadSync<MaterialInstance>(EmissiveInstance);
    const AssetResult<AssetHandle<MaterialInstance>> brick =
        assets.LoadSync<MaterialInstance>(BrickInstance);
    REQUIRE(translucent.has_value());
    REQUIRE(emissive.has_value());
    REQUIRE(brick.has_value());
    REQUIRE(translucent->Get()->GetDomain() == MaterialDomain::Translucent);
    REQUIRE(emissive->Get()->GetDomain() == MaterialDomain::Surface);

    // Renders a panel of the given domain and returns the downloaded output plus the panel target's
    // center texel (the >1.0 value the surface material samples).
    const auto RenderPanel = [&](GuiSurfaceDomain domain,
                                 const AssetHandle<MaterialInstance>& panelMaterial,
                                 vec4& targetCenterOut) -> vector<u8>
    {
        vector<Ref<Mesh>> meshes;
        Entity panelEntity;
        const Unique<Scene> scene = BuildPanelScene(Context, assets, Types, domain, panelMaterial,
                                                    *brick, meshes, panelEntity);

        const Unique<Viewport> viewport = MakeViewport(Context, assets);
        viewport->SetViewState({.World = scene.get(), .Camera = FrontCamera(), .Delta = 0.016f});
        Context.ImmediateCommands([&](CommandBuffer& cmd) { viewport->Render(cmd); });

        const GuiSurface& surface = scene->Get<GuiSurface>(panelEntity);
        REQUIRE(surface.GetTarget() != nullptr);
        CHECK(surface.WasRenderedLastDrive());
        const vector<u8> targetPixels = surface.GetTarget()->GetOutput()->GetImage()->Download();
        targetCenterOut = DecodeTexel(targetPixels, PanelRes.x, PanelRes.x / 2, PanelRes.y / 2);

        return viewport->GetOutput()->GetImage()->Download();
    };

    vec4 translucentTargetCenter{};
    const vector<u8> translucentOut =
        RenderPanel(GuiSurfaceDomain::Translucent, *translucent, translucentTargetCenter);
    vec4 emissiveTargetCenter{};
    const vector<u8> emissiveOut =
        RenderPanel(GuiSurfaceDomain::OpaqueEmissive, *emissive, emissiveTargetCenter);

    // (1) The document's >1.0 radiance survives into each panel's HDR target — the value the surface
    // material samples exceeds 1.0, so it will bloom through the scene's own bloom.
    CHECK(translucentTargetCenter.g > 1.0f);
    CHECK(translucentTargetCenter.b > 1.0f);
    CHECK(emissiveTargetCenter.g > 1.0f);
    CHECK(emissiveTargetCenter.b > 1.0f);

    // (2) The glow reaches the lit scene color: the panel center is bright in both domains.
    const vec4 translucentCenter = SampleBlock(translucentOut, Extent, vec2(0.5f, 0.5f));
    const vec4 emissiveCenter = SampleBlock(emissiveOut, Extent, vec2(0.5f, 0.5f));
    CHECK(translucentCenter.g + translucentCenter.b > 0.8f);
    CHECK(emissiveCenter.g + emissiveCenter.b > 0.8f);
    // The glow is cyan-biased (green/blue over red). The translucent panel returns the pure cyan
    // document texel, so its core stays cyan (green strictly over red). The opaque-emissive panel
    // adds the document's emissive into a lit bezel whose white-light red term, at the tonemapper's
    // fully-desaturated hot core, brings red up to meet green — the documented hot-core desaturation
    // — so green is never below red but ties there.
    CHECK(translucentCenter.g > translucentCenter.r);
    CHECK(emissiveCenter.g >= emissiveCenter.r);

    // (3) See-through vs occlude, at a point on the panel over a transparent document region.
    const vec4 translucentEdge = SampleBlock(translucentOut, Extent, vec2(0.74f, 0.5f), 2);
    const vec4 emissiveEdge = SampleBlock(emissiveOut, Extent, vec2(0.74f, 0.5f), 2);
    // Translucent: the transparent region shows the lit brick backdrop through the panel (its red
    // channel — which the cyan glow and its bloom never contribute to — is clearly present), proving
    // the panel writes no depth and does not occlude.
    CHECK(translucentEdge.r > 0.3f);
    // OpaqueEmissive: the opaque surface occludes the backdrop, so the same region is the dark bezel —
    // its red channel is far below the see-through panel's, since the backdrop is hidden.
    CHECK(emissiveEdge.r < translucentEdge.r - 0.2f);
}

TEST_CASE_FIXTURE(Veng::Test::GpuFixture,
                  "gui surface: the dirty-gate skips re-recording an idle panel")
{
    RegisterBuiltinTypes(Types);

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(CookGuiSurfacePack()).has_value());

    const AssetResult<AssetHandle<MaterialInstance>> translucent =
        assets.LoadSync<MaterialInstance>(TranslucentInstance);
    const AssetResult<AssetHandle<MaterialInstance>> brick =
        assets.LoadSync<MaterialInstance>(BrickInstance);
    REQUIRE(translucent.has_value());
    REQUIRE(brick.has_value());

    vector<Ref<Mesh>> meshes;
    Entity panelEntity;
    const Unique<Scene> scene =
        BuildPanelScene(Context, assets, Types, GuiSurfaceDomain::Translucent, *translucent, *brick,
                        meshes, panelEntity);

    const Unique<Viewport> viewport = MakeViewport(Context, assets);
    const auto RenderFrame = [&]()
    {
        viewport->SetViewState({.World = scene.get(), .Camera = FrontCamera(), .Delta = 0.016f});
        Context.ImmediateCommands([&](CommandBuffer& cmd) { viewport->Render(cmd); });
    };

    // The first frame materializes and records the document.
    RenderFrame();
    CHECK(scene->Get<GuiSurface>(panelEntity).WasRenderedLastDrive());

    // A second, unchanged frame does not re-record — the panel is idle (no binding moved, no
    // transition, no resolution change), so it keeps its persistent target content.
    RenderFrame();
    CHECK_FALSE(scene->Get<GuiSurface>(panelEntity).WasRenderedLastDrive());

    // A pixel-scale change moves target pixels without moving the layout extent, so the document's
    // own early-out would skip the re-record if the scale were not in the dirty gate beside the
    // extent.
    scene->Get<GuiSurface>(panelEntity).PixelScale = 2.0f;
    RenderFrame();
    CHECK(scene->Get<GuiSurface>(panelEntity).WasRenderedLastDrive());

    // The target doubled — and the content is magnified into it rather than drawn 1:1 in a corner,
    // which is what a target-only change produces. The glowing child covers the middle third, so at
    // 2x the document's own centre and a point three-quarters of the way across the target are both
    // inside it; at 1:1 in a corner the latter would be empty.
    const GuiSurface& scaled = scene->Get<GuiSurface>(panelEntity);
    REQUIRE(scaled.GetTarget() != nullptr);
    CHECK(scaled.GetTarget()->GetExtent() == PanelRes * 2u);
    const vector<u8> scaledPixels = scaled.GetTarget()->GetOutput()->GetImage()->Download();
    const uvec2 scaledExtent = PanelRes * 2u;
    const vec4 scaledCenter =
        DecodeTexel(scaledPixels, scaledExtent.x, scaledExtent.x / 2, scaledExtent.y / 2);
    CHECK(scaledCenter.g > 1.0f);
    // Two-thirds across is inside the magnified child (which spans [0.33, 0.67] of the target) and
    // outside an unmagnified one (which would span [0.165, 0.335]).
    const vec4 twoThirds =
        DecodeTexel(scaledPixels, scaledExtent.x, (scaledExtent.x * 5) / 8, scaledExtent.y / 2);
    CHECK(twoThirds.g > 1.0f);

    // The same scale on the next frame is idle again: the gate compares the scale, it does not
    // latch on having ever seen one.
    RenderFrame();
    CHECK_FALSE(scene->Get<GuiSurface>(panelEntity).WasRenderedLastDrive());

    // A structural change to the document dirties its layout, so the next frame re-records.
    Gui::Document* document = scene->Get<GuiSurface>(panelEntity).GetDocument();
    REQUIRE(document != nullptr);
    document->Add(document->Root(), Gui::ElementKind::Panel);
    RenderFrame();
    CHECK(scene->Get<GuiSurface>(panelEntity).WasRenderedLastDrive());
}

TEST_CASE_FIXTURE(
    Veng::Test::GpuFixture,
    "gui surface: a named driver is instantiated once and drives the panel each frame")
{
    RegisterBuiltinTypes(Types);
    g_Trace = DriverTrace{};

    AssetManager assets(Context, Tasks, Types);
    REQUIRE(assets.Mount(CookGuiSurfacePack()).has_value());

    const AssetResult<AssetHandle<MaterialInstance>> translucent =
        assets.LoadSync<MaterialInstance>(TranslucentInstance);
    const AssetResult<AssetHandle<MaterialInstance>> brick =
        assets.LoadSync<MaterialInstance>(BrickInstance);
    REQUIRE(translucent.has_value());
    REQUIRE(brick.has_value());

    vector<Ref<Mesh>> meshes;
    Entity panelEntity;
    const Unique<Scene> scene =
        BuildPanelScene(Context, assets, Types, GuiSurfaceDomain::Translucent, *translucent, *brick,
                        meshes, panelEntity);
    scene->Get<GuiSurface>(panelEntity).Driver = GuiDriverIdOf<PanelDriver>();

    GuiDriverRegistry drivers;
    drivers.Register<PanelDriver>();

    // The engine a driver fires sound through rides the viewport exactly like the driver catalog.
    const Unique<Audio::AudioDevice> audio =
        Audio::AudioDevice::Create(Audio::AudioDeviceInfo{.Backend = Audio::AudioBackend::Null});
    REQUIRE(audio != nullptr);

    const Unique<Viewport> viewport = MakeViewport(Context, assets);
    viewport->SetGuiDriverRegistry(&drivers);
    viewport->SetAudioEngine(&audio->GetEngine());
    const auto RenderFrame = [&](const f32 alpha)
    {
        viewport->SetViewState(
            {.World = scene.get(), .Camera = FrontCamera(), .Delta = 0.016f, .Alpha = alpha});
        Context.ImmediateCommands([&](CommandBuffer& cmd) { viewport->Render(cmd); });
    };

    RenderFrame(0.25f);
    RenderFrame(0.5f);

    // Instantiated once against the live document, updated every drive, and handed this viewport's
    // own frame: the entity the surface sits on, the render gather's interpolation alpha, its region.
    CHECK(g_Trace.Instantiates == 1);
    CHECK(g_Trace.Updates == 2);
    CHECK(g_Trace.Owner == panelEntity);
    CHECK(g_Trace.Alpha == doctest::Approx(0.5f));
    CHECK(g_Trace.RegionExtent == Extent);
    // And the services a driver loads and sounds through: the viewport's asset manager and engine.
    CHECK(g_Trace.Assets == &assets);
    CHECK(g_Trace.Audio == &audio->GetEngine());

    // The driver runs ahead of the document's record, so its style write is in the target the scene
    // samples: the child is red on both channels the fixture's cyan document never writes.
    const GuiSurface& surface = scene->Get<GuiSurface>(panelEntity);
    REQUIRE(surface.GetTarget() != nullptr);
    CHECK(surface.WasRenderedLastDrive());
    const vector<u8> pixels = surface.GetTarget()->GetOutput()->GetImage()->Download();
    const vec4 center = DecodeTexel(pixels, PanelRes.x, PanelRes.x / 2, PanelRes.y / 2);
    CHECK(center.r > 1.0f);
    CHECK(center.g < 0.01f);
}
