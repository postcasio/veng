#include <Veng/LevelOverlay.h>

#include <Veng/Application.h>
#include <Veng/Input.h>
#include <Veng/InputRouter.h>
#include <Veng/ManagedViewports.h>
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
        // managed world. Null when neither resolves (an overlay over a bare app with no managed
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

        SystemContext OverlaySystemContext(Application& app)
        {
            return SystemContext{.Assets = app.GetAssetManager(),
                                 .Input = app.GetInput(),
                                 .Tasks = app.GetTaskSystem()};
        }
    }

    LevelOverlay LevelOverlay::Open(Application& app, const LevelOverlayInfo& info)
    {
        AssetManager& assets = app.GetAssetManager();
        WorldRunner& runner = app.GetWorldRunner();

        // Residency: the source (and its world prefab) must be resident before the runner's LoadInto
        // asserts on it. WaitForResidency loads the source synchronously; otherwise the caller is
        // responsible for having preloaded it.
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

        // 1. Open an owned world through the runner (deferred start), running the populate hook against
        //    the freshly-spawned scene before the simulation starts, so a system's OnStart observes
        //    whatever the hook attached.
        overlay.m_World = runner.OpenWorld(WorldOpenInfo{
            .Source = source,
            .StartSimulation = false,
            .OnLoaded =
                [&info](WorldInstanceId, Scene& scene, ResidencyBatch&)
            {
                if (info.Populate)
                {
                    info.Populate(scene);
                }
            },
        });

        World& world = *runner.ResolveWorld(overlay.m_World);
        if (info.WaitForResidency)
        {
            world.Pending.WaitResident(app.GetTaskSystem());
        }
        Scene& scene = world.GetScene();

        // Map the level's render settings onto the renderer topology and seed the per-frame view
        // knobs carried into the engine's camera push.
        Renderer::SceneRendererSettings settings;
        if (const LevelRenderSettings* render = scene.TryGetFirst<LevelRenderSettings>())
        {
            overlay.m_Render = *render;
        }
        ApplyLevelRenderSettings(overlay.m_Render, settings, overlay.m_ViewKnobs);

        // 2. Create a Presented viewport for the region and register it last, so it composites over
        //    the covered world. A zero-extent region tracks the window (carries a Layout the
        //    compositor re-fits on resize); a fixed sub-region is placed absolutely.
        Renderer::Context& context = app.GetRenderContext();
        const bool trackWindow = info.Region.Extent == uvec2{};
        const Renderer::ViewportRegion region =
            trackWindow
                ? Renderer::ViewportRegion{.Offset = {0, 0}, .Extent = context.GetRenderExtent()}
                : info.Region;

        overlay.m_Viewport = Renderer::Viewport::Create({
            .Context = context,
            .Assets = assets,
            .Region = region,
            .Settings = settings,
            .Role = Renderer::ViewportRole::Presented,
            // Screen-space Gui documents lay out in logical points; feed the window content scale so
            // the overlay's HUD renders at logical size on a HiDPI display (the compositor re-stamps
            // it on resize alongside the region).
            .UiScale = context.IsHeadless() ? 1.0f : context.GetWindow().GetContentScale().x,
        });
        if (trackWindow)
        {
            overlay.m_Viewport->SetLayout(Renderer::ViewportLayout{});
        }
        app.RegisterViewport(*overlay.m_Viewport);

        // Bind the viewport to the overlay world so the managed-viewport presentation path pulls its
        // scene primary camera each frame (Entity::Null viewer) — the new home for what the manual
        // per-frame push did, with no game call.
        app.GetManagedViewports().RegisterBoundViewport(*overlay.m_Viewport, overlay.m_World,
                                                        Entity::Null, overlay.m_ViewKnobs);

        // 3. Route input across the three seams, capturing what each must restore.
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

        // 4. Pause the covered world for the overlay's lifetime, when one is named. The pause is a
        //    refcount, so stacked overlays over one world nest and an explicit game pause survives.
        if (info.CoveredWorld.IsValid())
        {
            overlay.m_PauseScope = runner.PauseScope(info.CoveredWorld);
        }

        // 5. Start the simulation — each system's OnStart fires with the populated scene.
        scene.StartSimulation(OverlaySystemContext(app));

        // Join the overlay stack so a higher overlay can resolve this one's scene from its seat.
        OpenOverlays().emplace_back(overlay.m_OverlaySeat, &scene);

        return overlay;
    }

    LevelOverlay::LevelOverlay(LevelOverlay&& other) noexcept
        : m_App(other.m_App), m_World(other.m_World), m_Viewport(std::move(other.m_Viewport)),
          m_Suspend(std::move(other.m_Suspend)),
          m_SuspendContext(std::move(other.m_SuspendContext)),
          m_PauseScope(std::move(other.m_PauseScope)), m_Render(other.m_Render),
          m_ViewKnobs(other.m_ViewKnobs), m_OverlaySeat(other.m_OverlaySeat),
          m_PriorCursorSeat(other.m_PriorCursorSeat)
    {
        other.m_App = nullptr;
    }

    LevelOverlay& LevelOverlay::operator=(LevelOverlay&& other) noexcept
    {
        if (this != &other)
        {
            Close();
            m_App = other.m_App;
            m_World = other.m_World;
            m_Viewport = std::move(other.m_Viewport);
            m_Suspend = std::move(other.m_Suspend);
            m_SuspendContext = std::move(other.m_SuspendContext);
            m_PauseScope = std::move(other.m_PauseScope);
            m_Render = other.m_Render;
            m_ViewKnobs = other.m_ViewKnobs;
            m_OverlaySeat = other.m_OverlaySeat;
            m_PriorCursorSeat = other.m_PriorCursorSeat;
            other.m_App = nullptr;
        }
        return *this;
    }

    LevelOverlay::~LevelOverlay()
    {
        Close();
    }

    void LevelOverlay::Close()
    {
        if (m_App == nullptr)
        {
            return;
        }

        Application& app = *m_App;
        WorldRunner& runner = app.GetWorldRunner();
        InputRouter& router = app.GetInputRouter();
        Scene& scene = runner.ResolveWorld(m_World)->GetScene();

        // Leave the overlay stack before the lower layer's contexts are restored, so it no longer
        // resolves as any seat's scene.
        std::erase_if(OpenOverlays(),
                      [&scene](const auto& entry) { return entry.second == &scene; });

        // Stop the simulation (each system's OnStop) while its scene is still live; CloseWorld below
        // only drops the world, it does not run OnStop.
        scene.StopSimulation(OverlaySystemContext(app));

        // Unwind the policy LIFO.
        // 1. Release the covered-world pause (refcount decrement; a no-op when none was held).
        m_PauseScope = WorldPauseScope{};

        // 2. Pop the focus scope (restores the suspended seat's contexts and pops its token).
        m_Suspend.reset();

        // 3. Restore the cursor seat and drop the overlay-seat pointer association by id (~Viewport
        //    does not clear it), and unregister the camera-pull binding, while the viewport is alive.
        router.SetCursorSeat(m_PriorCursorSeat);
        if (m_Viewport)
        {
            router.ClearViewportSeat(m_Viewport->GetId());
            app.GetManagedViewports().UnregisterBoundViewport(*m_Viewport);
        }

        // 4. Drop the viewport (self-unregisters from the compositor drive-list).
        m_Viewport.reset();

        // 5. Close the owned world (drops its scene).
        runner.CloseWorld(m_World);
        m_World = {};
        m_App = nullptr;
    }

    Scene& LevelOverlay::GetScene() const
    {
        VE_ASSERT(m_App != nullptr, "LevelOverlay::GetScene on a closed overlay");
        return m_App->GetWorldRunner().ResolveWorld(m_World)->GetScene();
    }

    Renderer::Viewport& LevelOverlay::GetViewport() const
    {
        VE_ASSERT(m_Viewport != nullptr, "LevelOverlay::GetViewport on a closed overlay");
        return *m_Viewport;
    }
}
