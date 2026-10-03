// The level-look funnel: every viewport configured from a level's authored LevelRenderSettings
// resolves it through Application::OnResolveGraphics before the one Configure that applies it.
//
// Drives a real headless Application through Run() with a resolver standing in for a consumer that
// layers a saved quality choice (ambient occlusion off, a smaller shadow map, a brighter exposure)
// over whatever the level authors. It pins:
//
//  - the bootstrap world seed configures the primary viewport with the resolved settings (the client
//    join start seeds through the same SeedViewportFromWorld, so it shares this path);
//  - a managed rebind onto a world authoring a look configures the viewport with the resolved
//    settings, in exactly one reconfigure;
//  - a LevelOverlay's open configures its viewport and view knobs with the resolved look;
//  - with no resolver (the identity default), each of those is the authored look unchanged;
//  - ApplyGraphicsSettings re-resolves both the managed viewport and an open overlay, and carries
//    the applied display calibration into the overlay's knobs.
//
// It needs a Context for the Application/viewports, so it rides the gpu band though it pins no pixels.

#include <doctest/doctest.h>

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
    // What the stand-in consumer's saved choices say, whatever the level authors.
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
    LevelRenderSettings AuthoredLook()
    {
        LevelRenderSettings look;
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

        // Opens an unstarted world with a camera and a seat; authors `look` into it when given.
        WorldInstanceId OpenCameraWorld(const std::optional<LevelRenderSettings>& look)
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
                scene.Add<LevelRenderSettings>(scene.CreateEntity(), *look);
            }
            return world;
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
            if (Resolver)
            {
                Resolver(input, output);
            }
        }
    };

    ApplicationInfo HeadlessInfo()
    {
        ApplicationInfo info;
        info.Name = "veng-level-look-resolve-test";
        info.Headless = true;
        info.ImGui = std::nullopt;
        info.HeadlessExtent = {128, 96};
        info.ManagedViewports = {ManagedViewportInfo{}};
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
    AssetHandle<Level> BuildOverlayLevel(AssetManager& assets, const TypeRegistry& types,
                                         const LevelRenderSettings& look)
    {
        vector<Prefab::PrefabEntity> entities;
        entities.push_back({{
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

    // What a viewport ended up configured with, captured inside the run.
    struct Applied
    {
        bool AO = false;
        u32 ShadowResolution = 0;
        f32 Exposure = 0.0f;
        u64 Reconfigures = 0;
    };

    Applied RunWorldSeed(bool withResolver)
    {
        TypeRegistry types;
        RegisterBuiltinTypes(types);
        SystemRegistry systems;

        // The fixture's level authors the default LevelRenderSettings: AO on, the default shadow map.
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
            if (frame == 0)
            {
                const Renderer::Viewport* viewport = app.GetManagedViewports().Get(0);
                REQUIRE(viewport != nullptr);
                applied.AO = viewport->GetSettings().AO;
                applied.ShadowResolution = viewport->GetSettings().ShadowResolution;
            }
        };
        app.Frames = 2;
        app.Run({});
        return applied;
    }

    Applied RunRebind(bool withResolver)
    {
        TypeRegistry types;
        RegisterBuiltinTypes(types);
        SystemRegistry systems;

        LookApp app(HeadlessInfo(), types, systems);
        if (withResolver)
        {
            app.Resolver = SavedOffResolver;
        }

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
                applied.AO = viewport->GetSettings().AO;
                applied.ShadowResolution = viewport->GetSettings().ShadowResolution;
                applied.Reconfigures = viewport->GetOutputGeneration() - generationBefore;
            }
        };
        app.Run({});
        return applied;
    }

    Applied RunOverlayOpen(bool withResolver)
    {
        TypeRegistry types;
        RegisterBuiltinTypes(types);
        SystemRegistry systems;

        LookApp app(HeadlessInfo(), types, systems);
        if (withResolver)
        {
            app.Resolver = SavedOffResolver;
        }

        Applied applied;
        app.StepFn = [&](LookApp& app, int frame)
        {
            if (frame == 1)
            {
                app.Overlay = LevelOverlay::Open(
                    app, LevelOverlayInfo{.Source = BuildOverlayLevel(app.GetAssetManager(), types,
                                                                      AuthoredLook())});
                const Renderer::SceneRendererSettings& settings =
                    app.Overlay->GetViewport().GetSettings();
                applied.AO = settings.AO;
                applied.ShadowResolution = settings.ShadowResolution;
                applied.Exposure = app.Overlay->GetViewState().Exposure;
            }
        };
        app.Run({});
        return applied;
    }
}

TEST_CASE("A consumer resolver survives the world seed; with none the seed is the authored look")
{
    const Applied resolved = RunWorldSeed(true);
    CHECK_FALSE(resolved.AO);
    CHECK(resolved.ShadowResolution == SavedShadowResolution);

    const Applied authored = RunWorldSeed(false);
    const LevelRenderSettings fixtureLook;
    CHECK(authored.AO == fixtureLook.AO);
    CHECK(authored.ShadowResolution == fixtureLook.ShadowResolution);
}

TEST_CASE("A consumer resolver survives a managed rebind, applied in one reconfigure")
{
    const Applied resolved = RunRebind(true);
    CHECK_FALSE(resolved.AO);
    CHECK(resolved.ShadowResolution == SavedShadowResolution);
    CHECK(resolved.Reconfigures == 1);

    const Applied authored = RunRebind(false);
    CHECK(authored.AO == AuthoredLook().AO);
    CHECK(authored.ShadowResolution == AuthoredLook().ShadowResolution);
    CHECK(authored.Reconfigures == 1);
}

TEST_CASE(
    "A consumer resolver configures a level overlay's open; with none it is the authored look")
{
    const Applied resolved = RunOverlayOpen(true);
    CHECK_FALSE(resolved.AO);
    CHECK(resolved.ShadowResolution == SavedShadowResolution);
    CHECK(resolved.Exposure == doctest::Approx(SavedExposure));

    const Applied authored = RunOverlayOpen(false);
    CHECK(authored.AO == AuthoredLook().AO);
    CHECK(authored.ShadowResolution == AuthoredLook().ShadowResolution);
    CHECK(authored.Exposure == doctest::Approx(AuthoredLook().Exposure));
}

TEST_CASE("ApplyGraphicsSettings re-resolves the managed viewport and an open overlay")
{
    TypeRegistry types;
    RegisterBuiltinTypes(types);
    SystemRegistry systems;

    LookApp app(HeadlessInfo(), types, systems);

    constexpr f32 Brightness = 1.3f;
    bool managedAOBefore = false;
    bool overlayAOBefore = false;
    Applied managed;
    Applied overlay;
    f32 overlayBrightness = 0.0f;

    app.InitFn = [&](LookApp& app)
    { app.GetManagedViewports().SetViewportWorld(0, app.OpenCameraWorld(AuthoredLook())); };
    app.StepFn = [&](LookApp& app, int frame)
    {
        if (frame == 1)
        {
            // Opened and applied under the identity resolver: both show the authored look.
            app.Overlay = LevelOverlay::Open(
                app, LevelOverlayInfo{.Source = BuildOverlayLevel(app.GetAssetManager(), types,
                                                                  AuthoredLook())});
            app.ApplyGraphicsSettings();
            managedAOBefore = app.GetManagedViewports().Get(0)->GetSettings().AO;
            overlayAOBefore = app.Overlay->GetViewport().GetSettings().AO;
        }
        else if (frame == 2)
        {
            // The player changes their choices and applies: one call reaches both viewports.
            app.Resolver = SavedOffResolver;
            app.GetGraphicsSettings().GetDisplay().Brightness = Brightness;
            app.ApplyGraphicsSettings();

            const Renderer::SceneRendererSettings& m =
                app.GetManagedViewports().Get(0)->GetSettings();
            managed.AO = m.AO;
            managed.ShadowResolution = m.ShadowResolution;
            const Renderer::SceneRendererSettings& o = app.Overlay->GetViewport().GetSettings();
            overlay.AO = o.AO;
            overlay.ShadowResolution = o.ShadowResolution;
            overlay.Exposure = app.Overlay->GetViewState().Exposure;
            overlayBrightness = app.Overlay->GetViewState().OutputBrightness;
        }
    };
    app.Run({});

    CHECK(managedAOBefore);
    CHECK(overlayAOBefore);
    CHECK_FALSE(managed.AO);
    CHECK(managed.ShadowResolution == SavedShadowResolution);
    CHECK_FALSE(overlay.AO);
    CHECK(overlay.ShadowResolution == SavedShadowResolution);
    CHECK(overlay.Exposure == doctest::Approx(SavedExposure));
    CHECK(overlayBrightness == doctest::Approx(Brightness));
}
