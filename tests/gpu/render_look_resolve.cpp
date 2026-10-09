// A scene's render look is a component every viewport presenting the scene resolves each frame.
//
// Drives a real headless Application through Run(). It pins:
//
//  - a per-frame look field written on the scene's RenderLook reaches the next frame's view with no
//    rebuild, and a topology field costs exactly one rebuild each way;
//  - two viewports presenting two worlds keep two looks;
//  - a consumer resolver composes over every viewport's look (an overlay's included), re-runs only
//    for the viewport whose look changed, and ApplyGraphicsSettings re-runs it once per viewport;
//  - a prefab carrying a RenderLook brings its look into a scene that had none;
//  - the bootstrap world, a managed rebind and an overlay each render their own look, through the
//    resolver when there is one and exactly as authored when there is none;
//  - the applied output calibration rides every managed and overlay push.
//
// It needs a Context for the Application/viewports, so it rides the gpu band though it pins no pixels.

#include <doctest/doctest.h>

#include <algorithm>
#include <optional>

#include <Veng/Application.h>
#include <Veng/LevelOverlay.h>
#include <Veng/ManagedViewports.h>
#include <Veng/World.h>
#include <Veng/WorldRunner.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Level.h>
#include <Veng/Asset/Prefab.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Render/GraphicsResolve.h>
#include <Veng/Render/GraphicsSettings.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Scene/BuiltinTypes.h>
#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SystemRegistry.h>

#include "support/BootstrapFixture.h"

using namespace Veng;

namespace
{
    // What the stand-in consumer's saved choices say, whatever the look authors.
    constexpr u32 SavedShadowResolution = 512;
    constexpr f32 SavedExposure = 3.0f;

    // A consumer resolver: the saved choices override the authored cost toggles and one view knob.
    void SavedOffResolver(const GraphicsResolveInput&, GraphicsResolveOutput& output)
    {
        output.Settings.AO = false;
        output.Settings.ShadowResolution = SavedShadowResolution;
        output.View.Exposure = SavedExposure;
    }

    // A look that differs from both the renderer defaults and the saved choices on every field the
    // checks read, so neither can be mistaken for the other.
    RenderLook AuthoredLook()
    {
        RenderLook look;
        look.AO = true;
        look.ShadowResolution = 2048;
        look.Exposure = 1.5f;
        return look;
    }

    // A headless application driven by closures, with a swappable resolve seam and an overlay slot
    // dropped before the engine tears down.
    class LookApp final : public Application
    {
    public:
        using Application::Application;

        ~LookApp() override { Overlay.reset(); }

        function<void(LookApp&)> InitFn;
        function<void(LookApp&, int)> StepFn;
        function<void(const GraphicsResolveInput&, GraphicsResolveOutput&)> Resolver;
        std::optional<LevelOverlay> Overlay;
        int Frames = 5;
        int Current = 0;
        int ResolveCalls = 0;

        // Opens an unstarted world with a camera and a seat; authors `look` into it when given.
        WorldInstanceId OpenCameraWorld(const std::optional<RenderLook>& look)
        {
            const WorldInstanceId world =
                GetWorldRunner().OpenWorld(WorldOpenInfo{.StartSimulation = false});
            Scene& scene = GetWorldRunner().ResolveWorld(world)->GetScene();
            const Entity camera = scene.CreateEntity();
            scene.Add<Transform>(camera).Position = vec3(0.0f, 0.0f, 5.0f);
            scene.Add<Camera>(camera);
            scene.Add<Viewer>(scene.CreateEntity()).Camera = camera;
            if (look.has_value())
            {
                scene.Add<RenderLook>(scene.CreateEntity(), *look);
            }
            return world;
        }

        // The look a world's scene carries; the world must author one.
        RenderLook& LookOf(WorldInstanceId world)
        {
            auto* look = GetWorldRunner().ResolveWorld(world)->GetScene().TryGetFirst<RenderLook>();
            REQUIRE(look != nullptr);
            return *look;
        }

    protected:
        void OnInitialize() override
        {
            if (InitFn)
            {
                InitFn(*this);
            }
        }

        void OnUpdate(f32) override
        {
            if (StepFn)
            {
                StepFn(*this, Current);
            }
            if (++Current >= Frames)
            {
                RequestExit();
            }
        }

        void OnResolveGraphics(const GraphicsResolveInput& input,
                               GraphicsResolveOutput& output) override
        {
            ++ResolveCalls;
            if (Resolver)
            {
                Resolver(input, output);
            }
        }
    };

    ApplicationInfo HeadlessInfo(usize viewports = 1)
    {
        ApplicationInfo info;
        info.Name = "veng-render-look-resolve-test";
        info.Headless = true;
        info.ImGui = std::nullopt;
        info.HeadlessExtent = {128, 96};
        info.ManagedViewports = vector<ManagedViewportInfo>(viewports);
        return info;
    }

    template <typename T>
    Prefab::Component Comp(const T& value, const TypeRegistry& types)
    {
        Prefab::Component component;
        component.Type = types.IdOf<T>();
        WriteFields(component.Record, &value, types.Info(component.Type), types);
        return component;
    }

    // A resident overlay level over a one-seat world prefab, authoring `look`.
    AssetHandle<Level> BuildOverlayLevel(const AssetManager& assets, const TypeRegistry& types,
                                         const RenderLook& look)
    {
        vector<Prefab::PrefabEntity> entities;
        entities.push_back(
            {.Components = {
                 Comp(Viewer{}, types),
                 Comp(InputContextStack{}, types),
                 Comp(PlayerInput{}, types),
                 Comp(SeatInput{.UsesKeyboardMouse = true, .Gamepad = GamepadId::None}, types),
                 Comp(Transform{}, types),
             }});
        const AssetHandle<Prefab> world =
            assets.Adopt<Prefab>(Prefab::Create(std::move(entities), {}));
        return assets.Adopt<Level>(Level::Create(world, {}, GameModeConfig{}, look));
    }

    // What a viewport ended up rendering with, captured inside the run.
    struct Applied
    {
        bool AO = false;
        u32 ShadowResolution = 0;
        f32 Exposure = 0.0f;
        u64 Reconfigures = 0;
    };

    Applied Rendered(const Renderer::Viewport& viewport)
    {
        return Applied{.AO = viewport.GetSettings().AO,
                       .ShadowResolution = viewport.GetSettings().ShadowResolution,
                       .Exposure = viewport.GetViewState().Exposure};
    }

    Applied RunWorldSeed(bool withResolver)
    {
        TypeRegistry types;
        RegisterBuiltinTypes(types);
        SystemRegistry systems;

        // The fixture's level authors the default RenderLook: AO on, the default shadow map.
        ApplicationInfo info = HeadlessInfo();
        info.World = GameWorldInfo{.Project = TestSupport::WriteBootstrapFixture(
                                       types, withResolver ? "look-seed" : "look-seed-identity")};
        LookApp app(std::move(info), types, systems);
        if (withResolver)
        {
            app.Resolver = SavedOffResolver;
        }

        Applied applied;
        app.StepFn = [&](LookApp& app, int frame)
        {
            // Frame 0's render is the first to present the world, so its look lands by frame 1.
            if (frame == 1)
            {
                const Renderer::Viewport* viewport = app.GetManagedViewports().Get(0);
                REQUIRE(viewport != nullptr);
                applied = Rendered(*viewport);
            }
        };
        app.Frames = 2;
        app.Run({});
        return applied;
    }
}

TEST_CASE("A look edit is live: per-frame fields rebuild nothing, a topology field rebuilds once")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;

    LookApp app(HeadlessInfo(), types, systems);

    WorldInstanceId world;
    u64 settled = 0;
    f32 editedExposure = 0.0f;
    u64 afterExposure = 0;
    bool bloomOff = true;
    u64 afterBloomOff = 0;
    bool bloomOn = false;
    u64 afterBloomOn = 0;
    Applied overlay;

    app.InitFn = [&](LookApp& app)
    {
        world = app.OpenCameraWorld(AuthoredLook());
        app.GetManagedViewports().SetViewportWorld(0, world);
    };
    app.StepFn = [&](LookApp& app, int frame)
    {
        const Renderer::Viewport& viewport = *app.GetManagedViewports().Get(0);
        if (frame == 1)
        {
            settled = viewport.GetOutputGeneration();
            app.LookOf(world).Exposure = 2.75f;
            app.Overlay = LevelOverlay::Open(
                app, LevelOverlayInfo{.Source = BuildOverlayLevel(app.GetAssetManager(), types,
                                                                  AuthoredLook())});
        }
        else if (frame == 2)
        {
            editedExposure = viewport.GetViewState().Exposure;
            afterExposure = viewport.GetOutputGeneration();
            overlay = Rendered(app.Overlay->GetViewport());
            app.LookOf(world).Bloom = false;
        }
        else if (frame == 3)
        {
            bloomOff = viewport.GetSettings().Bloom;
            afterBloomOff = viewport.GetOutputGeneration();
            app.LookOf(world).Bloom = true;
        }
        else if (frame == 4)
        {
            bloomOn = viewport.GetSettings().Bloom;
            afterBloomOn = viewport.GetOutputGeneration();
        }
    };
    app.Run({});

    CHECK(editedExposure == doctest::Approx(2.75f));
    CHECK(afterExposure == settled);
    CHECK_FALSE(bloomOff);
    CHECK(afterBloomOff == settled + 1);
    CHECK(bloomOn);
    CHECK(afterBloomOn == settled + 2);

    // With no resolver an overlay renders its level's look exactly as authored.
    CHECK(overlay.AO == AuthoredLook().AO);
    CHECK(overlay.ShadowResolution == AuthoredLook().ShadowResolution);
    CHECK(overlay.Exposure == doctest::Approx(AuthoredLook().Exposure));
}

TEST_CASE("Viewports compose a resolver over their own looks, re-run only on a change or an apply")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;

    LookApp app(HeadlessInfo(2), types, systems);

    // A resolver capping the shadow map at the saved choice, whatever each look authors.
    u32 cap = SavedShadowResolution;
    app.Resolver = [&cap](const GraphicsResolveInput&, GraphicsResolveOutput& output)
    { output.Settings.ShadowResolution = std::min(output.Settings.ShadowResolution, cap); };

    constexpr f32 Brightness = 1.3f;
    constexpr u32 LowerCap = 256;
    WorldInstanceId a;
    WorldInstanceId b;
    vector<Applied> first;
    int callsBeforeEdit = 0;
    int callsAfterEdit = 0;
    Applied edited;
    vector<u64> beforeApply;
    vector<Applied> applied;
    vector<u64> afterApply;
    vector<f32> brightness;

    RenderLook lookA;
    lookA.Exposure = 2.0f;
    lookA.ShadowResolution = 2048;
    RenderLook lookB;
    lookB.Exposure = 0.5f;
    lookB.ShadowResolution = 4096;
    RenderLook lookOverlay = AuthoredLook();

    app.InitFn = [&](LookApp& app)
    {
        a = app.OpenCameraWorld(lookA);
        b = app.OpenCameraWorld(lookB);
        app.GetManagedViewports().SetViewportWorld(0, a);
        app.GetManagedViewports().SetViewportWorld(1, b);
    };
    const auto viewports = [](LookApp& app)
    {
        return vector<const Renderer::Viewport*>{app.GetManagedViewports().Get(0),
                                                 app.GetManagedViewports().Get(1),
                                                 &app.Overlay->GetViewport()};
    };
    app.StepFn = [&](LookApp& app, int frame)
    {
        if (frame == 1)
        {
            app.Overlay = LevelOverlay::Open(
                app, LevelOverlayInfo{
                         .Source = BuildOverlayLevel(app.GetAssetManager(), types, lookOverlay)});
        }
        else if (frame == 2)
        {
            for (const Renderer::Viewport* viewport : viewports(app))
            {
                first.push_back(Rendered(*viewport));
            }
            callsBeforeEdit = app.ResolveCalls;
            app.LookOf(a).AO = false;
        }
        else if (frame == 3)
        {
            callsAfterEdit = app.ResolveCalls;
            edited = Rendered(*app.GetManagedViewports().Get(0));

            // The player lowers the cap and changes the display calibration, then applies.
            cap = LowerCap;
            app.GetGraphicsSettings().GetDisplay().Brightness = Brightness;
            app.ApplyGraphicsSettings();
            for (const Renderer::Viewport* viewport : viewports(app))
            {
                beforeApply.push_back(viewport->GetOutputGeneration());
            }
        }
        else if (frame == 4)
        {
            for (const Renderer::Viewport* viewport : viewports(app))
            {
                applied.push_back(Rendered(*viewport));
                afterApply.push_back(viewport->GetOutputGeneration());
                brightness.push_back(viewport->GetViewState().OutputBrightness);
            }
        }
    };
    app.Run({});

    // Two worlds, two looks — and the overlay its own.
    REQUIRE(first.size() == 3);
    CHECK(first[0].Exposure == doctest::Approx(lookA.Exposure));
    CHECK(first[1].Exposure == doctest::Approx(lookB.Exposure));
    CHECK(first[2].Exposure == doctest::Approx(lookOverlay.Exposure));
    for (const Applied& viewport : first)
    {
        CHECK(viewport.ShadowResolution == SavedShadowResolution);
    }

    // Only the edited look ran the resolver again, and the cap still holds over it.
    CHECK(callsAfterEdit == callsBeforeEdit + 1);
    CHECK_FALSE(edited.AO);
    CHECK(edited.ShadowResolution == SavedShadowResolution);

    // The apply re-resolved every viewport once, under the new cap and calibration.
    REQUIRE(applied.size() == 3);
    for (usize i = 0; i < applied.size(); ++i)
    {
        CHECK(applied[i].ShadowResolution == LowerCap);
        CHECK(afterApply[i] == beforeApply[i] + 1);
        CHECK(brightness[i] == doctest::Approx(Brightness));
    }
}

TEST_CASE("A prefab carrying a RenderLook brings its look into a scene that had none")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;

    LookApp app(HeadlessInfo(), types, systems);

    RenderLook look;
    look.Exposure = 3.5f;
    look.Bloom = false;

    WorldInstanceId world;
    f32 before = 0.0f;
    Applied after;
    bool bloom = true;

    app.InitFn = [&](LookApp& app)
    {
        world = app.OpenCameraWorld(std::nullopt);
        app.GetManagedViewports().SetViewportWorld(0, world);
    };
    app.StepFn = [&](LookApp& app, int frame)
    {
        const Renderer::Viewport& viewport = *app.GetManagedViewports().Get(0);
        if (frame == 1)
        {
            before = viewport.GetViewState().Exposure;
            vector<Prefab::PrefabEntity> entities;
            entities.push_back({.Components = {Comp(look, types)}});
            const AssetHandle<Prefab> prefab =
                app.GetAssetManager().Adopt<Prefab>(Prefab::Create(std::move(entities), {}));
            (void)prefab.Get()->SpawnInto(app.GetWorldRunner().ResolveWorld(world)->GetScene(),
                                          app.GetAssetManager());
        }
        else if (frame == 2)
        {
            after = Rendered(viewport);
            bloom = viewport.GetSettings().Bloom;
        }
    };
    app.Frames = 3;
    app.Run({});

    CHECK(before == doctest::Approx(Renderer::ViewState{}.Exposure));
    CHECK(after.Exposure == doctest::Approx(look.Exposure));
    CHECK_FALSE(bloom);
}

TEST_CASE("The bootstrap world renders its look through a resolver, or as authored with none")
{
    const Applied resolved = RunWorldSeed(true);
    CHECK_FALSE(resolved.AO);
    CHECK(resolved.ShadowResolution == SavedShadowResolution);
    CHECK(resolved.Exposure == doctest::Approx(SavedExposure));

    const Applied authored = RunWorldSeed(false);
    const RenderLook fixtureLook;
    CHECK(authored.AO == fixtureLook.AO);
    CHECK(authored.ShadowResolution == fixtureLook.ShadowResolution);
    CHECK(authored.Exposure == doctest::Approx(fixtureLook.Exposure));
}

TEST_CASE("A managed rebind renders the destination's look, applied in one reconfigure")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;

    LookApp app(HeadlessInfo(), types, systems);
    app.Resolver = SavedOffResolver;

    WorldInstanceId destination;
    u64 generationBefore = 0;
    Applied applied;
    app.InitFn = [&](LookApp& app)
    {
        app.GetManagedViewports().SetViewportWorld(0, app.OpenCameraWorld(std::nullopt));
        destination = app.OpenCameraWorld(AuthoredLook());
    };
    app.StepFn = [&](LookApp& app, int frame)
    {
        const Renderer::Viewport* viewport = app.GetManagedViewports().Get(0);
        REQUIRE(viewport != nullptr);
        if (frame == 1)
        {
            generationBefore = viewport->GetOutputGeneration();
            app.RebindManagedViewport(0, destination);
        }
        else if (frame == 3)
        {
            REQUIRE(app.GetManagedViewportWorld(0) == destination);
            applied = Rendered(*viewport);
            applied.Reconfigures = viewport->GetOutputGeneration() - generationBefore;
        }
    };
    app.Run({});

    CHECK_FALSE(applied.AO);
    CHECK(applied.ShadowResolution == SavedShadowResolution);
    CHECK(applied.Exposure == doctest::Approx(SavedExposure));
    CHECK(applied.Reconfigures == 1);
}
