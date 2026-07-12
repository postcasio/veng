#pragma once

#include <Veng/Veng.h>
#include <Veng/Assert.h>
#include <Veng/LaunchArguments.h>
#include <Veng/Window.h>
#include <Veng/Input.h>
#include <Veng/InputRouter.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Level.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/SceneCapture.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Renderer/GatherPass.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/SwapChainCompositePass.h>
#include <Veng/ImGui/ImGuiLayer.h>
#include <Veng/Gui/GuiConsumer.h>
#include <Veng/Net/PredictionHistory.h>
#include <Veng/Task/TaskSystem.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SimClock.h>
#include <Veng/Scene/SystemRegistry.h>

#include <span>

namespace Veng
{
    class ServerHost;
    class ClientHost;
    namespace Net
    {
        class Client;
    }

    /// @brief Returns the directory containing the running executable.
    ///
    /// A game mounts its asset pack relative to this so the launcher + module +
    /// pack resolve beside the binary, not at an absolute build-tree path.
    /// @return Absolute path to the directory holding the running binary.
    [[nodiscard]] VE_API path ExecutableDirectory();

    /// @brief A rectangle in normalized window fractions ([0,1]), resolved to pixels per resize.
    ///
    /// Where a managed viewport sits in the window, as fractions the engine resolves to pixel
    /// regions at construction and on every swapchain resize (round(Layout · render extent)). The
    /// default is the full window, so a single managed viewport with the default Layout covers the
    /// whole window exactly as an untracked one does. Two quadrant Layouts give resize-stable
    /// split-screen with no game code.
    struct ViewportLayout
    {
        /// @brief Top-left, as a fraction of the window ([0,1]).
        vec2 Offset = {0.0f, 0.0f};
        /// @brief Size, as a fraction of the window ([0,1]); full window by default.
        vec2 Extent = {1.0f, 1.0f};
    };

    /// @brief Opt-in configuration for one engine-owned managed viewport.
    ///
    /// Set ApplicationInfo::ManagedViewport (or an element of ApplicationInfo::ManagedViewports) to
    /// this to have Application construct, register, and drive one Presented viewport whose region
    /// follows its normalized Layout across resizes. A game flips this on and pushes its scene
    /// through GetManagedViewport(n)->SetViewState each frame (or names a Viewer for the engine to
    /// resolve), owning no SceneRenderer, sampler, texture, or composite. The editor leaves it unset.
    struct ManagedViewportInfo
    {
        /// @brief Render extent the managed viewport's SceneRenderer is sized to.
        ///
        /// Defaults to {} so the viewport tracks the window: its region follows the render-target
        /// extent (the swapchain framebuffer extent windowed, HeadlessExtent headless) and
        /// resizes with the swapchain, covering the whole window. A non-zero value pins a fixed
        /// render resolution that does not track resize.
        uvec2 Extent = {};
        /// @brief Output color format; resolved to Context::GetOutputFormat() when Undefined.
        Renderer::Format ColorFormat = Renderer::Format::Undefined;
        /// @brief Initial topology and sizing knobs for the managed viewport's SceneRenderer.
        Renderer::SceneRendererSettings Settings;
        /// @brief Initial render-resolution multiplier on the region extent (see Viewport).
        ///
        /// The managed viewport renders at its region extent times this; (0,1] renders below the
        /// window for dynamic resolution scaling, >1 supersamples. Must be > 0.
        f32 RenderScale = 1.0f;
        /// @brief Caps the managed viewport's allocation to this fraction of the window's pixels.
        ///
        /// The HiDPI supersample budget threaded into the viewport's ViewportInfo (see
        /// Renderer::ViewportInfo::MaxAllocationScale). The managed viewport tracks the full
        /// swapchain framebuffer extent — 2× the logical window on a HiDPI display — so 0.5 there
        /// brings the allocation back to logical-point resolution. 1.0 (the default) allocates at
        /// the full backing extent. Must be > 0.
        f32 MaxAllocationScale = 1.0f;
        /// @brief Enables automatic render-scale control on the managed viewport when set.
        ///
        /// The viewport eases its RenderScale toward this budget from measured GPU frame time
        /// each frame (see Viewport::SetDynamicResolution), rendering into a sub-rect of the fixed
        /// allocation; inert on a device without GPU timing. Unset leaves the scale fixed at
        /// RenderScale.
        optional<Renderer::DynamicResolutionSettings> DynamicResolution;
        /// @brief Where in the window this managed viewport sits; resolved to pixels per resize.
        ///
        /// Meaningful only when Extent tracks the window (the default empty Extent): the engine
        /// resolves the pixel region as round(Layout · render extent) at construction and on every
        /// swapchain resize. The default full-window Layout is byte-identical to a single untracked
        /// viewport covering the whole window. Ignored when Extent pins a fixed render resolution.
        ViewportLayout Layout;
        /// @brief Optional seat whose camera the engine resolves and pushes into this viewport.
        ///
        /// When set (and ApplicationInfo::World is on), the per-frame world drive resolves this
        /// seat's CameraView (ResolveCameraView) and pushes it into this viewport, generalizing the
        /// primary-camera push to a named seat. Entity::Null (the default) leaves the viewport for
        /// the game to drive itself through GetManagedViewport(n)->SetViewState.
        Entity Viewer = Entity::Null;
    };

    /// @brief Opt-in configuration for the engine-managed game world.
    ///
    /// Set ApplicationInfo::World to this to have Application bootstrap and drive the running
    /// game: it reads the cooked project file beside the executable, mounts each pack it names,
    /// loads the project's startup level, spawns it into a Scene (with the level's SceneSimulation
    /// attached), seeds the renderer from the level's render settings, and each frame ticks the
    /// simulation and pushes the resolved camera into the managed primary viewport. A game reaches
    /// the running world through GetWorld() and customizes it in OnWorldLoaded; the minimal game
    /// needs no code at all. Requires ManagedViewport to be set (the world renders through the
    /// managed viewport).
    struct GameWorldInfo
    {
        /// @brief Cooked project file to bootstrap from, resolved relative to the executable.
        ///
        /// Read from ExecutableDirectory() / Project; it names the packs to mount (each resolved
        /// beside the executable too) and the startup level the engine loads and runs.
        path Project;
        /// @brief Fixed simulation ticks per second the Sim phase steps at (the accumulator rate).
        ///
        /// The world drive accumulates frame time and steps the Sim phase at this fixed rate with a
        /// monotonic tick number, decoupling simulation from the frame rate; the View phase and render
        /// still run per frame, interpolating between the last two ticks. Must be positive.
        u32 SimTickRate = 60;
    };

    /// @brief Opt-in networking knobs for the engine-managed world; activation is a launch decision.
    ///
    /// Set ApplicationInfo::Net to tune the hosts the engine mounts when the launcher activates a net
    /// mode (`--server` opens a ServerHost on the managed world; `--join` connects a ClientHost). The
    /// knobs are only knobs — a game that leaves Net unset still gets these defaults when launched
    /// `--server`, so zero-config LAN hosting works with no configuration. A default (no net launch
    /// flag) constructs no host and behaves exactly as an offline app.
    struct GameNetInfo
    {
        /// @brief UDP port the server listens on, and the default a `--join` with no `:port` uses.
        u16 Port = 27750;
        /// @brief Maximum simultaneously accepted connections; a further request is denied.
        u32 MaxConnections = 16;
        /// @brief Emit a snapshot every this many sim ticks (2 ⇒ 30 Hz at a 60 Hz sim).
        u32 SnapshotIntervalTicks = 2;
        /// @brief How many recent input ticks each client input packet carries redundantly (the loss window).
        u32 InputRedundancyTicks = 3;
        /// @brief Client policy selecting the predicted entity set on a possession change; null uses the default.
        ///
        /// The engine passes it to the mounted ClientHost, which promotes this set to Tier::Predicted
        /// and re-runs the real Sim systems for it client-side each tick. Unset uses the
        /// owner-pawn-subtree default (the pawn plus its replicated attachments); a game widens it (a
        /// driven vehicle) or narrows it here. Inert off a client.
        PredictionPolicy PredictionPolicy;
    };

    /// @brief Construction parameters for Application.
    struct ApplicationInfo
    {
        /// @brief Application name passed to the Vulkan instance.
        string Name = "Veng Application";
        /// @brief Engine name passed to the Vulkan instance.
        string EngineName = "Veng";
        /// @brief Off-screen render-target extent used only when Headless.
        ///
        /// Headless runs borrow no window, so there is no swapchain to derive a render-target
        /// size from; this is the extent the render target (and the managed viewport) takes.
        /// Ignored windowed, where the swapchain framebuffer extent drives it instead.
        uvec2 HeadlessExtent{1280, 720};
        /// @brief Window creation parameters.
        WindowInfo WindowInfo;
        /// @brief ImGui integration; nullopt disables it for UI-free apps.
        ///
        /// Engaged by default. Force-disabled when Headless (ImGui requires a window).
        optional<ImGuiLayerInfo> ImGui = ImGuiLayerInfo{};
        /// @brief Run without a window, using an off-screen context; exits on RequestExit().
        bool Headless = false;
        /// @brief Requested display output mode for the swapchain (a preference; see DisplayMode).
        ///
        /// Defaults to picking the best available HDR mode, falling back to SDR. The resolved
        /// result is read back via Context::GetActiveDisplayMode().
        Renderer::DisplayMode RequestedDisplayMode = Renderer::DisplayMode::Auto;
        /// @brief Path for pipeline cache persistence; nullopt keeps the cache in-memory only.
        ///
        /// When set, seeds the pipeline cache from this file at startup (if it exists)
        /// and writes it back at shutdown. veng does not choose the path.
        optional<path> PipelineCachePath = std::nullopt;
        /// @brief Opt-in engine-owned managed primary viewport; nullopt leaves the app to own its views.
        ///
        /// When set, Application constructs one Presented viewport covering the window (default full
        /// Layout), registers it, tracks swapchain resize, and exposes it via GetPrimaryViewport().
        /// The plug-and-play path for a game. Convenience for a one-element ManagedViewports: when
        /// ManagedViewports is empty this becomes its sole entry. Unset (the editor) means
        /// GetPrimaryViewport() returns null.
        optional<ManagedViewportInfo> ManagedViewport = std::nullopt;
        /// @brief Opt-in engine-owned managed viewport set; the multi-viewport form of ManagedViewport.
        ///
        /// When non-empty, this is the managed set the engine constructs, registers, and drives —
        /// each with its normalized Layout resolved to pixels per resize, index 0 the primary. The
        /// singular ManagedViewport is sugar for a one-element vector: if this is empty and
        /// ManagedViewport is set, that one info becomes the sole managed viewport. Runtime changes
        /// go through ReconfigureManagedViewports.
        vector<ManagedViewportInfo> ManagedViewports;
        /// @brief Opt-in engine-managed game world; nullopt leaves the app to load and drive its own.
        ///
        /// When set (and ManagedViewport is too), Application mounts the named pack, loads the
        /// pack's startup level, owns the running Scene + SceneSimulation, and ticks + pushes the
        /// view each frame. Unset means the app loads and drives its own world (the editor, or a
        /// game wanting full control).
        optional<GameWorldInfo> World = std::nullopt;
        /// @brief Networking knobs for the engine-managed world; nullopt uses the zero-config defaults.
        ///
        /// Tunes the hosts the engine mounts when the launcher activates a net mode (`--server` /
        /// `--join`). Purely knobs — activation is a launch decision, not this field, so a windowed
        /// game with Net unset still hosts on the defaults when launched `--server`. Offline (no net
        /// flag) it is inert. Requires World to be set (net drives the managed world).
        optional<GameNetInfo> Net = std::nullopt;
    };

    /// @brief Base class for a veng application; subclass and override the lifecycle hooks.
    class Application
    {
    public:
        /// @brief Constructs the application with the given settings and borrowed registries.
        ///
        /// The TypeRegistry and SystemRegistry are borrowed, not owned: the host (launcher
        /// or cooker) constructs them and fills them via VengModuleRegister before this
        /// runs. Both must outlive this Application.
        /// @param info     Application creation parameters.
        /// @param types    Host-owned registry of reflected types; must outlive the app.
        /// @param systems  Host-owned registry of scene systems; must outlive the app.
        Application(ApplicationInfo info, TypeRegistry& types, SystemRegistry& systems);

        /// @brief Destroys the application, tearing down the pimpl'd net state.
        ///
        /// Out-of-line so the net-state pimpl (an incomplete type in this header) is complete at the
        /// destruction site in the translation unit.
        virtual ~Application();

        /// @brief Enter the main loop, blocking until the app exits.
        /// @param arguments  Command-line arguments forwarded from the launcher.
        void Run(vector<string> arguments);

        /// @brief Returns the application window.
        [[nodiscard]] Window& GetWindow() const { return *m_Window; }

        /// @brief Returns the launch arguments parsed from the command line at Run.
        ///
        /// Populated before Initialize; the engine already consumes the options it recognises
        /// (e.g. the startup-level override), so a game reads this only to inspect them itself.
        [[nodiscard]] const LaunchArguments& GetLaunchArguments() const { return m_LaunchArgs; }

        /// @brief Returns the frame-coherent input service.
        ///
        /// Always present, updated once per frame before OnUpdate/OnRender so per-frame
        /// edges and deltas reflect the current frame. A headless run reports the neutral
        /// all-zeros state rather than being absent.
        [[nodiscard]] Input& GetInput() const { return *m_Input; }

        /// @brief Returns the input router that routes window events to ImGui and the Input snapshot.
        ///
        /// Push InputFocus::Gameplay to give the running game exclusive input (and capture the
        /// cursor); pop it (or the Shift+Esc release chord) to return input to the UI. Always
        /// present; headless borrows no window and routes nothing.
        [[nodiscard]] InputRouter& GetInputRouter() const { return *m_InputRouter; }

        /// @brief Returns the render context.
        [[nodiscard]] Renderer::Context& GetRenderContext() { return m_RenderContext; }

        /// @brief Returns the task system.
        ///
        /// @pre Run() has initialized the engine — the task system exists only inside Run().
        [[nodiscard]] TaskSystem& GetTaskSystem()
        {
            VE_ASSERT(m_TaskSystem, "GetTaskSystem before Run(): the task system exists only once "
                                    "Run() has initialized the engine");
            return *m_TaskSystem;
        }

        /// @brief Returns the asset manager.
        ///
        /// @pre Run() has initialized the engine — the asset manager exists only inside Run().
        [[nodiscard]] AssetManager& GetAssetManager()
        {
            VE_ASSERT(m_AssetManager, "GetAssetManager before Run(): the asset manager exists only "
                                      "once Run() has initialized the engine");
            return *m_AssetManager;
        }

        /// @brief Returns the host-owned, process-wide registry of reflected types.
        ///
        /// Borrowed: the host constructs it, pre-registers builtins, and calls
        /// VengModuleRegister before passing it here. Must outlive this Application.
        [[nodiscard]] TypeRegistry& GetTypeRegistry() { return m_TypeRegistry; }

        /// @brief Returns the host-owned, process-wide registry of scene systems.
        ///
        /// Borrowed: the host constructs it and calls VengModuleRegister before passing
        /// it here, so a module's SceneSystem registrations are present. A SceneSimulation
        /// reads it to instantiate the running systems. Must outlive this Application.
        [[nodiscard]] SystemRegistry& GetSystemRegistry() { return m_SystemRegistry; }

        /// @brief Returns the ImGui layer, or nullptr if the app opted out.
        [[nodiscard]] ImGuiLayer* GetImGuiLayer() const { return m_ImGuiLayer.get(); }

        /// @brief Returns the Gui router consumer that routes UI input into attached documents.
        ///
        /// Registered second in the router registry (behind ImGui) and always present. It walks the
        /// engine's registered viewports for a hosted, interactive document under the pointer or the
        /// focused seat; a document is display-only until the game makes it interactive
        /// (Gui::Document::SetInteractive) while holding its seat.
        [[nodiscard]] Gui::GuiConsumer& GetGuiConsumer() const { return *m_GuiConsumer; }

        /// @brief Registers a viewport into the engine drive-list rendered each frame.
        ///
        /// Stores a non-owning pointer in registration order (which is render order — a producer
        /// viewport registered before its consumer renders first); the caller keeps the owning
        /// Unique from Viewport::Create. The engine hands the viewport a back-reference, so
        /// dropping that Unique self-unregisters it (~Viewport erases its own pointer). Must not
        /// be called from inside the per-frame drive loop. Double-registering a viewport is a
        /// fatal assert.
        /// @param viewport  The viewport to drive; its lifetime stays with the caller.
        void RegisterViewport(Renderer::Viewport& viewport);

        /// @brief Registers a scene capture into the engine drive-list rendered each frame.
        ///
        /// Captures render ahead of every viewport, so a material sampling a capture's output
        /// reads this frame's result — the capture-side analogue of the viewport
        /// registration-order RTT contract. The same ownership model as RegisterViewport: the
        /// caller keeps the owning Unique from SceneCapture::Create, and dropping it
        /// self-unregisters. Double-registering a capture is a fatal assert.
        /// @param capture  The capture to drive; its lifetime stays with the caller.
        void RegisterCapture(Renderer::SceneCapture& capture);

        /// @brief Registers a scene into the engine simulation drive-list ticked each frame.
        ///
        /// The tick counterpart of RegisterViewport: the engine ticks every registered scene's
        /// SceneSimulation (in registration order, while started and not paused) and drives every
        /// registered scene's CaptureSurface components — so a secondary scene (an overlay, an
        /// offscreen feed, a view-less background) is engine-driven the same way the primary world is.
        /// Ticking is decoupled from rendering: registration alone gates it, so a scene no viewport
        /// presents still ticks. The same ownership model as RegisterViewport: the caller keeps the
        /// owning Unique<Scene>, and dropping it self-unregisters (~Scene erases its own pointer). The
        /// first registered scene is the primary simulation (SetWorldPaused targets it).
        /// Double-registering a scene is a fatal assert.
        /// @param scene  The scene to tick and drive; its lifetime stays with the caller.
        void RegisterSimulation(Scene& scene);

        /// @brief Returns the number of engine-owned managed viewports.
        ///
        /// Zero when ApplicationInfo::ManagedViewport / ManagedViewports is unset; one for the
        /// plug-and-play default; more after a split-screen ReconfigureManagedViewports.
        /// @return The managed viewport count.
        [[nodiscard]] usize GetManagedViewportCount() const { return m_ManagedViewports.size(); }

        /// @brief Returns the engine-owned managed viewport at an index, or null when out of range.
        ///
        /// The game pushes its scene and camera through the returned viewport's SetViewState each
        /// frame (or the engine resolves a bound Viewer for it). Index 0 is the primary.
        /// @param index  The managed viewport index.
        /// @return The managed viewport, or nullptr when index is out of range.
        [[nodiscard]] Renderer::Viewport* GetManagedViewport(usize index) const
        {
            return index < m_ManagedViewports.size() ? m_ManagedViewports[index].Viewport.get()
                                                     : nullptr;
        }

        /// @brief Returns the engine-owned managed primary viewport, or null when unconfigured.
        ///
        /// Equivalent to GetManagedViewport(0). Non-null only when a managed viewport is configured.
        /// The game pushes its scene and camera through the returned viewport's SetViewState each
        /// frame. Stays index 0 across a ReconfigureManagedViewports.
        /// @return The managed primary viewport, or nullptr.
        [[nodiscard]] Renderer::Viewport* GetPrimaryViewport() const
        {
            return GetManagedViewport(0);
        }

        /// @brief Rebuilds the engine-managed viewport set at a safe point.
        ///
        /// Records the requested set and applies it at the top of the next frame, before any system
        /// iteration — never mid-iteration, mirroring the SetRegion resize debounce. The apply drops
        /// removed viewports (RAII self-unregister), constructs added ones, registers them, and
        /// resolves each Layout to pixels; index 0 remains the primary. Split-screen is a two-element
        /// reconfigure. Requires a managed viewport to have been configured at startup.
        /// @param viewports  The new managed set; each info's Layout, Viewer, and render knobs apply.
        void ReconfigureManagedViewports(std::span<const ManagedViewportInfo> viewports);

        /// @brief Returns the managed primary viewport's debug-draw accumulator, or null when unconfigured.
        ///
        /// The single-viewport convenience for the canonical per-SceneView DebugDraw channel: it
        /// forwards to GetPrimaryViewport()->GetDebugDraw(). Null when no managed viewport is
        /// configured (ApplicationInfo::ManagedViewport unset), in which case a caller owning its
        /// own Viewport reaches the accumulator through that viewport directly. The debug-draw pass
        /// renders only when the viewport's SceneRendererSettings::DebugDraw is enabled.
        /// @return The primary viewport's DebugDraw accumulator, or nullptr.
        [[nodiscard]] Renderer::DebugDraw* GetDebugDraw() const
        {
            const Renderer::Viewport* primary = GetPrimaryViewport();
            return primary ? &primary->GetDebugDraw() : nullptr;
        }

        /// @brief Returns the engine-managed game world's Scene, or null when unmanaged.
        ///
        /// Non-null only when ApplicationInfo::World is set, after the world is bootstrapped. The
        /// Scene owns the level's SceneSimulation (Scene::GetSimulation); a game reads and edits
        /// the world through it. In client mode (`--join`) this is the scene the ClientHost loaded
        /// from the join flow, so it is null until the accept lands and the world starts.
        /// @return The managed world's Scene, or nullptr.
        [[nodiscard]] Scene* GetWorld() const { return m_PrimaryWorld; }

        /// @brief Returns this peer's network role: Client under `--join`, Server otherwise.
        ///
        /// Server for a standalone app, a listen server, and a dedicated server; Client only when the
        /// launcher activated join mode. Threaded onto every Sim tick's SystemContext so the authority
        /// filter gates state advancement.
        /// @return The peer's NetRole.
        [[nodiscard]] NetRole GetNetRole() const;

        /// @brief Returns the mounted server host, or null when not hosting.
        ///
        /// Non-null only after a `--server` launch bootstraps the managed world. A game reaches it for
        /// its own traffic or to drain the lifecycle events (ServerHost::Events) its pawn-cleanup rule
        /// watches; the join glue itself needs no game code.
        /// @return The server host, or nullptr.
        [[nodiscard]] ServerHost* GetServerHost() const;

        /// @brief Returns the mounted client host, or null when not joined.
        ///
        /// Non-null only after a `--join` launch. A game reaches it to inspect its own seat / possessed
        /// pawn; the per-frame join drive is the engine's.
        /// @return The client host, or nullptr.
        [[nodiscard]] ClientHost* GetClientHost() const;

        /// @brief Returns the level the managed world was bootstrapped from, or an empty handle.
        ///
        /// Valid only with a managed world; a game reads the level's render settings or game-mode
        /// config from it (e.g. to seed its own editable render-settings copy).
        /// @return The managed world's level handle.
        [[nodiscard]] const AssetHandle<Level>& GetWorldLevel() const { return m_WorldLevel; }

        /// @brief Returns the per-frame view knobs the managed world pushes into the primary viewport.
        ///
        /// Seeded from the level's render settings at bootstrap; a game edits it in place (the
        /// tone/bloom/environment knobs a render-settings UI mutates) and the engine fills in the
        /// scene/camera/delta each frame before pushing. Meaningless without a managed world.
        /// @return The mutable managed-world ViewState.
        [[nodiscard]] Renderer::ViewState& GetWorldViewState() { return m_WorldView; }

        /// @brief Pauses or resumes the primary simulation's per-frame tick.
        ///
        /// Back-compat sugar over the primary simulation's pause (SceneSimulation::SetPaused): the
        /// state lives on the simulation, not on Application. Paused, the engine still pushes the view
        /// each frame (the camera resolves and the scene renders) and still drives the scene's
        /// captures, but runs no simulation tick — the path a fixed-pose capture or a game pause menu
        /// takes. The primary simulation is the first registered (the managed world, registered at
        /// bootstrap); a no-op when none is registered.
        /// @param paused  True to stop ticking the primary simulation, false to resume.
        void SetWorldPaused(bool paused);

        /// @brief Returns whether the primary simulation's tick is paused.
        ///
        /// Reads the primary simulation's pause (SceneSimulation::IsPaused); false when no simulation
        /// is registered.
        [[nodiscard]] bool IsWorldPaused() const;

        /// @brief Returns the current fixed simulation tick number.
        ///
        /// Monotonic, advanced by the world drive's accumulator; the same number every registered
        /// scene's Sim phase steps through. Zero before the first tick runs and while fully paused.
        [[nodiscard]] u64 GetSimTick() const { return m_SimClock.GetTick(); }

        /// @brief Returns this frame's interpolation fraction into the next Sim tick, in [0, 1).
        ///
        /// The residual accumulator the render gather and View systems blend the last two ticks by.
        /// A game driving its own viewport (or a LevelOverlay) pushes this into its ViewState so its
        /// scene interpolates in phase with the primary world.
        [[nodiscard]] f32 GetSimAlpha() const { return m_SimAlpha; }

    protected:
        /// @brief Called once after all engine systems are initialized.
        virtual void OnInitialize() {}

        /// @brief Called once after the managed world is loaded, before its simulation starts.
        ///
        /// Only fires when ApplicationInfo::World is set. The Scene is spawned and the renderer is
        /// seeded from the level by this point, but the simulation has not started — a game seeds
        /// its own editable render-settings copy, captures input focus, or waits on @p pending
        /// before a deterministic capture here. Default is a no-op (the minimal game needs none).
        /// @param world    The managed world's Scene (its SceneSimulation attached but not started).
        /// @param pending  The world spawn's not-yet-resident assets; wait on it before a capture.
        virtual void OnWorldLoaded(Scene& world, ResidencyBatch& pending) {}

        /// @brief Called once per frame before rendering.
        /// @param delta  Time in seconds since the previous frame.
        virtual void OnUpdate(f32 delta) {}

        /// @brief Called once per frame to record draw commands.
        virtual void OnRender() {}

        /// @brief Called after the main loop exits and the GPU is idle, before context teardown.
        ///
        /// Release every engine resource held by the application here (reset Refs/Uniques,
        /// AssetHandles included) — resources that outlive the context fail on destruction.
        virtual void OnDispose() {}

        /// @brief Called in client mode when the own seat's possessed pawn changes (or clears).
        ///
        /// Only fires under `--join`, from the join drive, when the replicated own seat's Possesses
        /// resolves to a newly-bound pawn (Entity::Null when it possesses none) — the point a game
        /// points its Local-tier camera/viewer at that pawn. The camera rig stays untouched
        /// client-local View machinery; this only names its target. Default is a no-op.
        /// @param world  The client scene the ClientHost loaded.
        /// @param pawn   The pawn the own seat now possesses, or Entity::Null.
        virtual void OnClientPossession(Scene& world, Entity pawn) {}

        /// @brief Signals the run loop to exit after the current frame.
        ///
        /// The only way to stop a headless app; also works for windowed apps.
        void RequestExit() { m_ShouldExit = true; }

    private:
        /// @brief One engine-owned managed viewport plus the info it was built from.
        ///
        /// The owning Unique self-unregisters from m_Viewports on drop; the retained Info supplies
        /// the normalized Layout the resize callback re-resolves and the optional bound Viewer the
        /// world drive resolves a camera for.
        struct ManagedViewport
        {
            /// @brief The owned, registered Presented viewport.
            Unique<Renderer::Viewport> Viewport;
            /// @brief The info this viewport was constructed from (Layout, Viewer, render knobs).
            ManagedViewportInfo Info;
        };

        /// @brief The pimpl'd network state (hosts + input buffers); defined in Application.cpp.
        struct NetState;

        void Initialize();
        void Frame();

        /// @brief Mounts the world pack, loads its startup level, and starts the running world.
        ///
        /// Called at the end of Initialize when ApplicationInfo::World is set: mounts the pack
        /// beside the executable, reads its cooked startup level, seeds the managed viewport from
        /// the level's render settings, spawns the world (LoadInto), fires OnWorldLoaded, then
        /// starts the scene's simulation. In client mode the startup-level load is deferred to the
        /// join flow (the level comes from the accept), so this only mounts the packs and connects.
        /// A missing pack or startup level is a fatal assert.
        void BootstrapWorld();

        /// @brief Seeds the managed viewport + view knobs from a started world scene's render settings.
        ///
        /// The shared tail of bringing a world online: applies the scene's LevelRenderSettings onto the
        /// primary viewport's topology and the per-frame view. Used by the server/standalone bootstrap
        /// and by the client when its join-loaded scene starts.
        /// @param world  The world scene to seed the viewport from.
        void SeedViewportFromWorld(Scene& world);

        /// @brief Opens the ServerHost on the started managed world (`--server`).
        ///
        /// Constructs the NetState Server arm from ApplicationInfo::Net (or the zero-config defaults):
        /// listens on the configured port, accepts up to MaxConnections, and replicates at the snapshot
        /// interval. Called from the bootstrap tail after m_World starts.
        /// @param levelId  The startup level id folded into each ConnectAccept for the client to load.
        void StartServer(AssetId levelId);

        /// @brief Connects the Net::Client and mounts the ClientHost (`--join`).
        ///
        /// Constructs the NetState Client arm from the launch target + ApplicationInfo::Net: opens the
        /// connection and installs the join hooks (LoadClientLevel, prefab resolve, OnClientPossession).
        /// The world scene is not loaded here — it arrives through the join flow (LoadClientLevel).
        void ConnectClient();

        /// @brief Loads the accepted level into a fresh client scene, server-authoritative entities skipped.
        ///
        /// The ClientHost's LoadLevel hook: LoadSync the level, LoadInto a scene with
        /// SkipServerAuthoritative (the authored server entities arrive from the stream), retaining the
        /// residency batch for the deferred OnWorldLoaded. The host owns the returned scene.
        /// @param id  The level AssetId the accept named.
        /// @return The freshly-loaded client scene.
        [[nodiscard]] Unique<Scene> LoadClientLevel(AssetId id);

        /// @brief Registers a just-loaded world scene as the primary simulation and starts it.
        ///
        /// The client-mode counterpart of the server/standalone bootstrap tail: registers @p world as
        /// simulation #0, seeds the viewport, fires OnWorldLoaded (with the retained client residency
        /// batch), and starts the simulation with this peer's role. Runs once, when the ClientHost's
        /// join flow has loaded the scene.
        /// @param world  The join-loaded client scene the ClientHost owns.
        void StartWorldScene(Scene& world);

        /// @brief Pumps the mounted net host for one frame: join/accept, apply the stream, feed input.
        ///
        /// The receive+send half of the net world drive, called once per frame after the sim ticks:
        /// server-side it stamps the scene's change tick, pumps the ServerHost (accept → spawn seats,
        /// generate + flush the stream, reap), ingests each connection's redundant input into its jitter
        /// buffer, and reaps a departed connection's buffer; client-side it pumps the ClientHost (join,
        /// apply spawn/despawn + snapshots, wire the own seat), starts the world scene once it loads,
        /// and sends this frame's stamped local input. A no-op with no net host.
        void PumpNet();

        /// @brief Feeds each ready connection's input scheduled for a server tick into its seat.
        ///
        /// Server-only: consumes the input each connection's client stamped at @p tick from its jitter
        /// buffer (falling back to the underrun coast when it has not arrived) and writes it into the
        /// connection's seat PlayerInput, so the control system re-derives Intent from the wire input at
        /// the matching tick. Called once per Sim step, ahead of the systems. A no-op off the server.
        /// @param tick  The server sim tick whose matching client input is consumed.
        void FeedServerSeatInputs(u64 tick);

        /// @brief Stamps this client tick's resolved local input into the input send window.
        ///
        /// Client-only: records the local input seat's resolved PlayerInput for @p clientTick into the
        /// redundant send buffer (drained once per frame by PumpNet). A no-op off a client, or with no
        /// local input seat in the scene.
        /// @param clientTick  The client sim tick being stamped.
        void StampClientInput(u64 clientTick);

        /// @brief Builds the managed viewport set from a list of infos, registering each.
        ///
        /// Replaces m_ManagedViewports: constructs one Presented Viewport per info at its
        /// Layout-resolved region, applies its dynamic-resolution opt-in, and registers it in order
        /// (index 0 the primary). The prior set's Uniques are dropped first, self-unregistering.
        /// @param infos  The managed viewport infos to build from.
        void BuildManagedViewports(std::span<const ManagedViewportInfo> infos);

        /// @brief Resolves an info's normalized Layout to a pixel region against the render extent.
        ///
        /// round(Layout · GetRenderExtent()) when the info tracks the window (empty Extent); a fixed
        /// region at the pinned Extent otherwise. The single source the constructor and the resize
        /// callback both use, so a viewport's region and its resize tracking agree.
        /// @param info  The managed viewport info to resolve.
        /// @return The pixel region for the viewport.
        [[nodiscard]] Renderer::ViewportRegion
        ResolveManagedRegion(const ManagedViewportInfo& info) const;

        /// @brief Pushes the managed world's render source into one managed viewport.
        ///
        /// A viewport naming a Viewer resolves that seat's CameraView (ResolveCameraView) at the
        /// viewport's aspect; one with no bound Viewer takes the scene's primary camera (PushSceneView).
        /// Fills the per-frame view knobs and pushes via SetViewState. Called per managed viewport
        /// each frame when a managed world is running.
        /// @param managed  The managed viewport (and its bound Viewer) to push into.
        /// @param delta    Frame delta in seconds, forwarded to the renderer.
        void PushManagedViewportView(const ManagedViewport& managed, f32 delta);

        /// @brief This frame's pointer routing paired with the one scene it is scoped to.
        ///
        /// Entity handles are scene-local, so a routing carries the scene its Owner seat belongs to;
        /// the per-sim assembly hands the routing only to the sim whose scene this names, every other
        /// sim getting an empty routing.
        struct ScopedPointer
        {
            /// @brief The resolved routing (owner seat + region-local position), empty when unscoped.
            PointerRouting Routing;
            /// @brief The scene the routing applies to, or null when no sim receives it this frame.
            const Scene* Scene = nullptr;
        };

        /// @brief Resolves this frame's pointer routing and the one scene it is scoped to.
        ///
        /// Reads the pointer's window point from the Input snapshot and identifies the viewport that
        /// owns it (ResolvePointerViewport): while captured the cursor seat's viewport, else the
        /// associated viewport under the free cursor. The routing is scoped to that viewport's
        /// presented scene, resolving the owner seat scene-locally so no cross-scene handle leaks;
        /// while captured with no associated viewport it falls back to the primary world. Fed into
        /// each ticked sim's SystemContext so only the owning scene's InputMappingSystem sees the
        /// pointer.
        /// @return The routing and the scene it applies to, or an empty routing scoped to no scene.
        [[nodiscard]] ScopedPointer ComputePointerRouting() const;

        /// @brief Builds a ticked simulation's SystemContext, resolving its primary presenting viewport.
        ///
        /// Fills the always-present services (assets, input, tasks) and the given per-scene @p pointer,
        /// then resolves the sim's primary presenting viewport — the first registered Presented
        /// viewport whose retained scene is @p scene — to populate View (its retained camera + region +
        /// UI scale) and Debug (its debug-draw sink). View is nullopt and Debug null for a view-less
        /// sim.
        /// @param scene    The scene being ticked.
        /// @param pointer  This frame's routing for @p scene (empty when the pointer is elsewhere).
        /// @param tick     The tick number to stamp (the Sim step, or the last completed tick in View).
        /// @param alpha    The interpolation fraction to stamp (0 in Sim, the frame residual in View).
        /// @param firstStepThisFrame  True on the frame's first Sim step (false in View); resets a
        ///                            per-frame accumulator (see SystemContext::FirstStepThisFrame).
        /// @return The assembled per-tick context.
        [[nodiscard]] SystemContext BuildSystemContext(const Scene& scene,
                                                       const PointerRouting& pointer, u64 tick,
                                                       f32 alpha, bool firstStepThisFrame) const;

        /// @brief Returns the primary simulation (the first registered scene's), or null when none.
        [[nodiscard]] SceneSimulation* PrimarySimulation() const;

        /// @brief Re-resolves every window-tracking managed viewport's region from its Layout.
        ///
        /// The swapchain-invalidation callback: recomputes each managed viewport's region as
        /// round(Layout · GetRenderExtent()) so quadrant regions track the window; SetRegion
        /// debounces each SceneRenderer::Resize to the next Render. Pinned viewports are untouched.
        void ResolveManagedViewportLayouts();

        /// @brief Constructs the managed gather pass and swapchain composite tail.
        ///
        /// Called at init only when ImGui is present; wires the swapchain-invalidation re-target
        /// and the initial graph compiles.
        void InitializeManagedTail();

        /// @brief Gathers the registered Presented viewports and composites them into the swapchain.
        ///
        /// Rebinds the gather's placement list ({ output, region } per Presented viewport — slots
        /// rebound only when a viewport's output view identity changed), runs the gather, then the
        /// composite. Called from Frame after ImGuiLayer::Render. No-op without the managed tail.
        /// @param cmd  The command buffer to record into.
        void RenderManagedTail(Renderer::CommandBuffer& cmd);

        /// @brief Discovers and drives every registered scene's CaptureSurface components.
        ///
        /// Iterates every registered simulation's scene (regardless of started/paused state —
        /// registration alone gates capture driving) for Renderer::CaptureSurface components,
        /// materializing each one's SceneCapture on first sight and registering it on the capture
        /// drive-list (so it renders with the imperatively-registered captures), pushing this frame's
        /// capture source from the entity's world position per the component's refresh policy, and
        /// binding the capture output onto the sibling MeshRenderer's material. The capture
        /// self-unregisters when its component/entity/scene is destroyed. Called from Frame ahead of
        /// the capture render loop. No-op with no registered simulation.
        void DriveCaptureSurfaces();

        ApplicationInfo m_Info;

        /// @brief Command-line arguments parsed once in Run, before Initialize.
        LaunchArguments m_LaunchArgs;

        /// @brief Borrowed from the host; must outlive this app and every Scene it creates.
        TypeRegistry& m_TypeRegistry;

        /// @brief Borrowed from the host; must outlive this app and every SceneSimulation it drives.
        SystemRegistry& m_SystemRegistry;

        Unique<Window> m_Window;

        /// @brief Frame-coherent input; borrows m_Window, so constructed after and reset before it.
        Unique<Input> m_Input;

        Renderer::Context m_RenderContext;

        /// @brief Routes window events to ImGui + Input by focus; borrows the window, input, and
        ///        ImGui layer, so it is constructed after them and reset before them.
        Unique<InputRouter> m_InputRouter;

        /// @brief Worker pool; destroyed after m_AssetManager to avoid tearing down live workers.
        Unique<TaskSystem> m_TaskSystem;

        /// @brief Constructed after m_RenderContext; reset before teardown so assets retire safely.
        Unique<AssetManager> m_AssetManager;

        Unique<ImGuiLayer> m_ImGuiLayer;

        /// @brief The Gui router consumer, registered second (behind ImGui) so attached documents
        ///        receive UI-owned input. Borrows the router/input/window/m_Viewports, so it is
        ///        declared after them (destroyed before them, since it is registered on the router).
        Unique<Gui::GuiConsumer> m_GuiConsumer;

        /// @brief Non-owning, ordered list of viewports the engine renders each frame.
        ///
        /// Registration order is render order. Holds raw pointers; each registered Viewport holds
        /// a back-reference and erases itself on destruction (order-preserving). A subclass's
        /// panel-owned viewports destruct before this base member, so the back-reference is live.
        vector<Renderer::Viewport*> m_Viewports;

        /// @brief Non-owning, ordered list of scene captures rendered ahead of the viewports.
        ///
        /// The viewport drive-list's capture sibling: raw pointers, registration order, each
        /// capture self-unregistering on destruction through its back-reference.
        vector<Renderer::SceneCapture*> m_Captures;

        /// @brief Non-owning, ordered list of scenes the engine ticks and drives each frame.
        ///
        /// Registration order is tick order; index 0 is the primary simulation. Holds raw pointers,
        /// each registered Scene holding a back-reference and erasing itself on destruction
        /// (order-preserving). The managed world registers first at bootstrap; overlays register and
        /// deregister around their lifetimes.
        vector<Scene*> m_Simulations;

        /// @brief The engine-owned managed viewport set; empty when no managed viewport is configured.
        ///
        /// Index 0 is the primary. Constructed at Initialize from ApplicationInfo, rebuilt by a
        /// deferred ReconfigureManagedViewports at the top of a frame.
        vector<ManagedViewport> m_ManagedViewports;

        /// @brief A pending managed-viewport reconfigure, applied at the top of the next frame.
        ///
        /// Set by ReconfigureManagedViewports, consumed (and cleared) before any system iteration in
        /// Frame, so the rebuild never runs mid-iteration. nullopt when none is pending.
        optional<vector<ManagedViewportInfo>> m_PendingReconfigure;

        /// @brief The engine-managed game world's Scene (sim attached); null when World is unset.
        ///
        /// Owns the standalone/server managed world. Null in client mode, where the ClientHost owns the
        /// join-loaded scene and m_PrimaryWorld points at it instead.
        Unique<Scene> m_World;
        /// @brief The active primary world scene (m_World, or the client host's join-loaded scene).
        ///
        /// The one scene the world drive ticks and pushes into the managed viewport. Set when the world
        /// comes online (bootstrap for server/standalone, the join flow for a client); null before then.
        Scene* m_PrimaryWorld = nullptr;
        /// @brief The pimpl'd net hosts + input buffers; null unless a net launch mode is active.
        Unique<NetState> m_Net;
        /// @brief The client scene's spawn residency, held from LoadClientLevel until StartWorldScene.
        ResidencyBatch m_ClientPending;
        /// @brief The level the managed world was bootstrapped from; empty when World is unset.
        AssetHandle<Level> m_WorldLevel;
        /// @brief Per-frame view knobs pushed into the managed viewport; seeded from the level.
        Renderer::ViewState m_WorldView;

        /// @brief The managed gather pass assembling the Presented viewports; present only with ImGui.
        Unique<Renderer::GatherPass> m_Gather;
        /// @brief The managed swapchain composite tail; present only with ImGui.
        Unique<Renderer::SwapChainCompositePass> m_Composite;
        /// @brief Compiled gather graph, re-Compile()d on swapchain resize.
        Unique<Renderer::CompiledGraph> m_GatherGraph;
        /// @brief Compiled composite graph, re-Compile()d on swapchain resize.
        Unique<Renderer::CompiledGraph> m_CompositeGraph;

        /// @brief Last placement list pushed to the gather; rebinds only when it changes.
        ///
        /// Guards against per-frame bindless churn: the gather's slots are re-registered only on a
        /// frame where a Presented viewport's output view identity or region differs from this.
        vector<Renderer::CompositePlacement> m_GatheredPlacements;

        /// @brief The fixed-timestep accumulator driving every registered simulation's Sim phase.
        ///
        /// Frame time folds in here; it advances the shared tick number by the whole fixed steps it
        /// completes and reports the residual interpolation alpha. Reconfigured to the managed world's
        /// SimTickRate at bootstrap; 60 Hz otherwise.
        SimClock m_SimClock;

        /// @brief This frame's interpolation fraction (GetSimAlpha), retained for the view pushes.
        f32 m_SimAlpha = 0.0f;

        /// @brief Whether the previous frame latched input (an active sim ran zero Sim ticks).
        ///
        /// The edge latch: BeginFrame skips the pressed/released edge roll on a frame following one
        /// that had a live simulation but ran no tick, so an edge survives to the next tick-running
        /// frame. A frame with no active simulation (the editor, a full pause) does not latch — its
        /// input rolls every frame like an ordinary UI, so this stays false there. Starts false so
        /// the first frame rolls cleanly.
        bool m_PreviousFrameLatchedInput = false;

        bool m_ShouldExit = false;
    };
}
