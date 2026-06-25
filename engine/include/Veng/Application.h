#pragma once

#include <Veng/Veng.h>
#include <Veng/Window.h>
#include <Veng/Input.h>
#include <Veng/InputRouter.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Renderer/GatherPass.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/SwapChainCompositePass.h>
#include <Veng/ImGui/ImGuiLayer.h>
#include <Veng/Task/TaskSystem.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/SystemRegistry.h>

namespace Veng
{
    /// @brief Returns the directory containing the running executable.
    ///
    /// A game mounts its asset pack relative to this so the launcher + module +
    /// pack resolve beside the binary, not at an absolute build-tree path.
    /// @return Absolute path to the directory holding the running binary.
    [[nodiscard]] VE_API path ExecutableDirectory();

    /// @brief Opt-in configuration for the engine-owned managed primary viewport.
    ///
    /// Set ApplicationInfo::ManagedViewport to this to have Application construct, register, and
    /// drive one Presented viewport whose region tracks the whole window. A game flips this on and
    /// pushes its scene through GetPrimaryViewport()->SetViewState each frame, owning no
    /// SceneRenderer, sampler, texture, or composite. The editor leaves it unset.
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
        /// each frame (see Viewport::SetDynamicResolution); inert on a device without GPU timing.
        /// Unset leaves the scale fixed at RenderScale.
        optional<Renderer::DynamicResolutionSettings> DynamicResolution;
        /// @brief Enables the outer-loop allocation-tier controller on the managed viewport when set.
        ///
        /// Folds the per-frame RenderScale into a long EMA and steps a quantized allocation tier so
        /// the allocation follows the sustained sub-rect (see Viewport::SetDynamicResolution).
        /// Honored only when DynamicResolution is also set (the inner loop feeds the outer loop);
        /// unset leaves the allocation at the inner loop's MaxScale.
        optional<Renderer::AllocationTierSettings> AllocationTier;
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
        /// When set, Application constructs one Presented viewport covering the window, registers
        /// it, tracks swapchain resize, and exposes it via GetPrimaryViewport(). The plug-and-play
        /// path for a game. Unset (the editor) means GetPrimaryViewport() returns null.
        optional<ManagedViewportInfo> ManagedViewport = std::nullopt;
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
        virtual ~Application() = default;

        /// @brief Enter the main loop, blocking until the app exits.
        /// @param arguments  Command-line arguments forwarded from the launcher.
        void Run(vector<string> arguments);

        /// @brief Returns the application window.
        [[nodiscard]] Window& GetWindow() const { return *m_Window; }

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
        [[nodiscard]] TaskSystem& GetTaskSystem() { return *m_TaskSystem; }

        /// @brief Returns the asset manager.
        [[nodiscard]] AssetManager& GetAssetManager() { return *m_AssetManager; }

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

        /// @brief Returns the engine-owned managed primary viewport, or null when unconfigured.
        ///
        /// Non-null only when ApplicationInfo::ManagedViewport is set. The game pushes its scene
        /// and camera through the returned viewport's SetViewState each frame.
        /// @return The managed primary viewport, or nullptr.
        [[nodiscard]] Renderer::Viewport* GetPrimaryViewport() const
        {
            return m_PrimaryViewport.get();
        }

    protected:
        /// @brief Called once after all engine systems are initialized.
        virtual void OnInitialize() {}

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

        /// @brief Signals the run loop to exit after the current frame.
        ///
        /// The only way to stop a headless app; also works for windowed apps.
        void RequestExit() { m_ShouldExit = true; }

    private:
        void Initialize();
        void Frame();

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

        ApplicationInfo m_Info;

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

        /// @brief Non-owning, ordered list of viewports the engine renders each frame.
        ///
        /// Registration order is render order. Holds raw pointers; each registered Viewport holds
        /// a back-reference and erases itself on destruction (order-preserving). A subclass's
        /// panel-owned viewports destruct before this base member, so the back-reference is live.
        vector<Renderer::Viewport*> m_Viewports;

        /// @brief The engine-owned managed primary viewport; null when ManagedViewport is unset.
        Unique<Renderer::Viewport> m_PrimaryViewport;

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

        bool m_ShouldExit = false;
    };
}
