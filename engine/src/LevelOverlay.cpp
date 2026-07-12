#include <Veng/LevelOverlay.h>

#include <Veng/Application.h>
#include <Veng/Input.h>
#include <Veng/InputRouter.h>
#include <Veng/Input/SeatFocusScope.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/InputMappingContext.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneViewport.h>
#include <Veng/Window.h>

#include <utility>

namespace Veng
{
    namespace
    {
        // The open overlays' (seat, scene) pairs, in open order — the layer stack a new overlay's
        // SuspendSeat resolves its scene from, so a stacked overlay suspends the input contexts of
        // the overlay directly beneath it. The base layer beneath the first overlay is the managed
        // world, which no overlay owns and so is not in this list.
        vector<std::pair<Entity, Scene*>>& OpenOverlays()
        {
            static vector<std::pair<Entity, Scene*>> overlays;
            return overlays;
        }

        // An engine-owned empty input-mapping context the suspend scope swaps the layer beneath's
        // input to. It carries a sentinel id so the focus scope treats it as a real swap-in (the
        // scope swaps only for a valid-id context) and resolves no actions, so the suspended seat
        // goes neutral while the overlay is up. Each overlay owns its own, released on close.
        AssetHandle<InputMappingContext> MakeSuspendContext()
        {
            constexpr AssetId SuspendContextId{.Value = 0x5E00'0000'0000'0001ULL};
            auto entry = CreateRef<Detail::AssetCacheEntry>(Detail::AssetCacheEntry{
                .Id = SuspendContextId,
                .Type = AssetTypeTrait<InputMappingContext>::Type,
                .Resource = std::static_pointer_cast<void>(InputMappingContext::Create({}, {})),
            });
            AssetHandle<InputMappingContext> handle;
            Detail::RehydrateHandleField(&handle, SuspendContextId, std::move(entry));
            return handle;
        }

        // The scene the seat lives in: a lower overlay's scene when the seat names one, else the
        // primary world. Null when neither resolves (an overlay over a bare app with no managed
        // world and no lower overlay), which makes the suspend scope's context swap an inert no-op.
        Scene* SceneOfSeat(Application& app, const Entity seat)
        {
            if (seat == Entity::Null)
            {
                return nullptr;
            }
            for (const auto& [overlaySeat, scene] : OpenOverlays())
            {
                if (overlaySeat == seat)
                {
                    return scene;
                }
            }
            const World* managed = app.GetWorldRunner().ResolveWorld(app.GetManagedWorldId());
            return managed != nullptr ? &managed->GetScene() : nullptr;
        }
    }

    LevelOverlay LevelOverlay::Open(Application& app, const LevelOverlayInfo& info)
    {
        AssetManager& assets = app.GetAssetManager();

        // Residency: the source (and its world prefab) must be resident before LoadInto asserts on
        // it. WaitForResidency loads the source synchronously and blocks on the spawn's batch;
        // otherwise the caller is responsible for having preloaded it.
        AssetHandle<Level> source = info.Source;
        if (info.WaitForResidency)
        {
            const AssetResult<AssetHandle<Level>> loaded = assets.LoadSync<Level>(source.Id());
            VE_ASSERT(loaded.has_value(), "LevelOverlay::Open: source level {} failed to load: {}",
                      source.Id().Value, loaded.has_value() ? "" : loaded.error().Detail);
            source = *loaded;
        }
        VE_ASSERT(source.IsLoaded(),
                  "LevelOverlay::Open: source level is not resident — preload it or set "
                  "WaitForResidency");

        LevelOverlay overlay;
        overlay.m_App = &app;
        overlay.m_PausePrimarySim = info.PausePrimarySim;

        // 1. Load the level into a fresh scene + simulation (not started).
        overlay.m_Instance = source->LoadInto(assets, app.GetSystemRegistry());
        if (info.WaitForResidency)
        {
            overlay.m_Instance.Pending.WaitResident(app.GetTaskSystem());
        }
        Scene& scene = *overlay.m_Instance.World;

        // 2. Run the populate hook against the loaded scene, before the simulation starts, so a
        //    system's OnStart observes whatever the hook attached.
        if (info.Populate)
        {
            info.Populate(scene);
        }

        // 3. Create a Presented viewport for the region and register it last, so it composites over
        //    the primary. A zero-extent region means the full window and tracks resizes.
        Renderer::Context& context = app.GetRenderContext();
        const uvec2 renderExtent = context.GetRenderExtent();
        overlay.m_TrackWindow = info.Region.Extent == uvec2{};
        overlay.m_Region = overlay.m_TrackWindow
                               ? Renderer::ViewportRegion{.Offset = {0, 0}, .Extent = renderExtent}
                               : info.Region;

        // Map the level's render settings onto the renderer topology and seed the persistent per-frame
        // view knobs. The knobs persist across frames (Update pushes this same instance) so a debug
        // panel can retune them, mirroring the managed world's GetWorldViewState template.
        Renderer::SceneRendererSettings settings;
        if (const LevelRenderSettings* render = scene.TryGetFirst<LevelRenderSettings>())
        {
            overlay.m_Render = *render;
        }
        ApplyLevelRenderSettings(overlay.m_Render, settings, overlay.m_ViewKnobs);

        overlay.m_Viewport = Renderer::Viewport::Create({
            .Context = context,
            .Assets = assets,
            .Region = overlay.m_Region,
            .Settings = settings,
            .Role = Renderer::ViewportRole::Presented,
            // Screen-space Gui documents lay out in logical points; feed the window content scale so
            // the overlay's HUD renders at logical size on a HiDPI display (Update re-applies it).
            .UiScale = context.IsHeadless() ? 1.0f : context.GetWindow().GetContentScale().x,
        });
        app.RegisterViewport(*overlay.m_Viewport);

        // Register the overlay scene as a simulation so the engine ticks it and drives its captures —
        // the tick is the engine's now, not a manual TickSimulation in Update. Push the initial view
        // right away (mirroring BootstrapWorld's seed) so the viewport's retained scene pointer is set
        // before the first engine tick, closing the first-frame gap in the per-sim view resolution.
        app.RegisterSimulation(scene);
        PushSceneView(*overlay.m_Viewport, scene, overlay.m_ViewKnobs);

        // 4. Route input across the three seams, capturing what each must restore.
        InputRouter& router = app.GetInputRouter();
        const InputSeat seat = ResolveInputSeat(&scene);
        overlay.m_OverlaySeat = seat.Viewer;

        // Pointer: a free pointer over the overlay's region routes to the overlay seat.
        router.AssociateViewportSeat(*overlay.m_Viewport, seat.Viewer);

        // Cursor/keyboard: the captured cursor and keyboard/device window events follow the overlay
        // seat now; the prior cursor seat is restored on close.
        overlay.m_PriorCursorSeat = router.GetCursorSeat();
        router.SetCursorSeat(seat.Viewer);

        // Suspend the layer beneath: a viewport-less focus scope over SuspendSeat (defaulting to the
        // seat that was the cursor seat) pushes a focus token and swaps that seat's input contexts to
        // an engine-owned empty context, so the suspended seat resolves no actions while the overlay
        // is up. The scope is the overlay's own seat association's counterpart, not the same seat.
        const Entity suspendSeat =
            info.SuspendSeat != Entity::Null ? info.SuspendSeat : overlay.m_PriorCursorSeat;
        overlay.m_SuspendContext = MakeSuspendContext();
        const InputSeat suspend{.Viewer = suspendSeat, .World = SceneOfSeat(app, suspendSeat)};
        overlay.m_Suspend =
            CreateUnique<SeatFocusScope>(router, suspend, nullptr, overlay.m_SuspendContext);

        // Optionally freeze the base world the overlay covers (the runner's first-opened world —
        // bootstrap opens the managed world first). Input focus and sim pause are separate knobs: the
        // observed pause value is captured and restored on close, so stacking and a
        // game-paused-before-open base both survive.
        if (info.PausePrimarySim)
        {
            const vector<Unique<World>>& worlds = app.GetWorldRunner().GetWorlds();
            overlay.m_PausedWorld = worlds.empty() ? WorldInstanceId{} : worlds.front()->Id;
            overlay.m_PriorPaused = app.IsWorldPaused(overlay.m_PausedWorld);
            app.SetWorldPaused(overlay.m_PausedWorld, true);
        }

        // 5. Start the simulation — each system's OnStart fires with the populated scene.
        overlay.m_Instance.World->StartSimulation(
            SystemContext{.Assets = assets, .Input = app.GetInput(), .Tasks = app.GetTaskSystem()});
        overlay.m_Started = true;

        // Join the overlay stack so a higher overlay can resolve this one's scene from its seat.
        OpenOverlays().emplace_back(overlay.m_OverlaySeat, overlay.m_Instance.World.get());

        return overlay;
    }

    LevelOverlay::LevelOverlay(LevelOverlay&& other) noexcept
        : m_App(other.m_App), m_Instance(std::move(other.m_Instance)),
          m_Viewport(std::move(other.m_Viewport)), m_Suspend(std::move(other.m_Suspend)),
          m_SuspendContext(std::move(other.m_SuspendContext)), m_Render(other.m_Render),
          m_ViewKnobs(other.m_ViewKnobs), m_Region(other.m_Region),
          m_OverlaySeat(other.m_OverlaySeat), m_PriorCursorSeat(other.m_PriorCursorSeat),
          m_PausePrimarySim(other.m_PausePrimarySim), m_PriorPaused(other.m_PriorPaused),
          m_PausedWorld(other.m_PausedWorld), m_TrackWindow(other.m_TrackWindow),
          m_Started(other.m_Started)
    {
        other.m_App = nullptr;
        other.m_Started = false;
    }

    LevelOverlay& LevelOverlay::operator=(LevelOverlay&& other) noexcept
    {
        if (this != &other)
        {
            Close();
            m_App = other.m_App;
            m_Instance = std::move(other.m_Instance);
            m_Viewport = std::move(other.m_Viewport);
            m_Suspend = std::move(other.m_Suspend);
            m_SuspendContext = std::move(other.m_SuspendContext);
            m_Render = other.m_Render;
            m_ViewKnobs = other.m_ViewKnobs;
            m_Region = other.m_Region;
            m_OverlaySeat = other.m_OverlaySeat;
            m_PriorCursorSeat = other.m_PriorCursorSeat;
            m_PausePrimarySim = other.m_PausePrimarySim;
            m_PriorPaused = other.m_PriorPaused;
            m_PausedWorld = other.m_PausedWorld;
            m_TrackWindow = other.m_TrackWindow;
            m_Started = other.m_Started;
            other.m_App = nullptr;
            other.m_Started = false;
        }
        return *this;
    }

    LevelOverlay::~LevelOverlay()
    {
        Close();
    }

    void LevelOverlay::Update(const f32 delta)
    {
        if (m_App == nullptr || !m_Viewport)
        {
            return;
        }

        Renderer::Context& context = m_App->GetRenderContext();

        // The engine ticks the overlay's simulation (it is a registered sim) and scopes its pointer
        // to the overlay's own viewport region, so Update only re-applies the region and pushes the
        // view — no manual TickSimulation or pointer routing here.

        // Re-apply the region against the current framebuffer extent so a full-window overlay tracks
        // resizes (the viewport is not in Application's resize-tracked list); a fixed region is
        // re-applied unchanged (a no-op).
        if (m_TrackWindow)
        {
            m_Region = {.Offset = {0, 0}, .Extent = context.GetRenderExtent()};
        }
        m_Viewport->SetRegion(m_Region);

        // Track the window content scale each frame so the overlay's screen-space HUD stays at
        // logical size across a HiDPI display or a move to a differently-scaled monitor (the same
        // GetContentScale() the pointer routing above uses, so layout, draw, and hit-testing agree).
        m_Viewport->SetUiScale(context.IsHeadless() ? 1.0f
                                                    : context.GetWindow().GetContentScale().x);

        // Push the resolved camera over the persistent view knobs; the viewport's own render drives
        // the scene's GuiOverlay HUD. The overlay shares the frame's interpolation alpha with the
        // primary world (the same accumulator drives both), so its scene interpolates in phase.
        PushSceneView(*m_Viewport, *m_Instance.World, m_ViewKnobs, delta, m_App->GetSimAlpha());
    }

    void LevelOverlay::Close()
    {
        if (m_App == nullptr)
        {
            return;
        }

        Application& app = *m_App;
        InputRouter& router = app.GetInputRouter();

        // 1. Stop the simulation (each system's OnStop). The engine ticked it while registered; the
        //    scene reset in step 5 self-unregisters it from the drive-list.
        if (m_Started && m_Instance.World)
        {
            m_Instance.World->StopSimulation(SystemContext{.Assets = app.GetAssetManager(),
                                                           .Input = app.GetInput(),
                                                           .Tasks = app.GetTaskSystem()});
            m_Started = false;
        }

        // Leave the overlay stack before the lower layer's contexts are restored, so it no longer
        // resolves as any seat's scene.
        std::erase_if(OpenOverlays(),
                      [this](const auto& entry) { return entry.second == m_Instance.World.get(); });

        // 2. Restore the observed world-pause value (not a blind false), so a stacked pause or a
        //    game-paused-before-open primary survives.
        if (m_PausePrimarySim)
        {
            app.SetWorldPaused(m_PausedWorld, m_PriorPaused);
        }

        // 3. Pop the focus scope (restores the suspended seat's contexts and pops its token).
        m_Suspend.reset();

        // 4. Restore the cursor seat and drop the overlay-seat pointer association by id (~Viewport
        //    does not clear it).
        router.SetCursorSeat(m_PriorCursorSeat);
        if (m_Viewport)
        {
            router.ClearViewportSeat(m_Viewport->GetId());
        }

        // 5. Drop the viewport (self-unregisters from the drive-list).
        m_Viewport.reset();
        m_Instance.World.reset();
        m_App = nullptr;
    }

    Scene& LevelOverlay::GetScene() const
    {
        VE_ASSERT(m_Instance.World != nullptr, "LevelOverlay::GetScene on a closed overlay");
        return *m_Instance.World;
    }

    Renderer::Viewport& LevelOverlay::GetViewport() const
    {
        VE_ASSERT(m_Viewport != nullptr, "LevelOverlay::GetViewport on a closed overlay");
        return *m_Viewport;
    }
}
