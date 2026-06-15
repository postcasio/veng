#pragma once

#include <Veng/Veng.h>
#include <Veng/Window.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Renderer/Context.h>
#include <Veng/ImGui/ImGuiLayer.h>
#include <Veng/Task/TaskSystem.h>

namespace Veng
{
    // The directory containing the running executable (for the launcher, the
    // launcher binary). A game mounts its asset pack relative to this so the
    // launcher + module + pack are a relocatable trio resolved beside the binary,
    // not at an absolute build-tree path. path-typed, so it pulls in no backend
    // include — include_hygiene stays green.
    [[nodiscard]] VE_API path ExecutableDirectory();

    struct ApplicationInfo
    {
        string Name = "Veng Application";
        string EngineName = "Veng";
        uvec2 InternalRenderExtent{1280, 720};
        WindowInfo WindowInfo;
        // ImGui is opt-in: engaged (the default) creates an ImGuiLayer; set to
        // nullopt for a UI-free app that pays nothing for ImGui at runtime.
        // (ImGui needs a window, so it is force-disabled when Headless.)
        optional<ImGuiLayerInfo> ImGui = ImGuiLayerInfo{};
        // Headless: create no window and a windowless (off-screen) context. The
        // run loop then runs until RequestExit() rather than the window closing.
        bool Headless = false;
        // When set, the render context seeds its pipeline cache from this file at
        // startup (if it exists) and writes the cache back here at shutdown.
        // nullopt (default) keeps the cache in-memory only — no file is read or
        // written. The app owns the path; veng does not choose a cache directory.
        optional<path> PipelineCachePath = std::nullopt;
    };

    class Application
    {
    public:
        explicit Application(ApplicationInfo info);
        virtual ~Application() = default;

        void Run(vector<string> arguments);

        [[nodiscard]] Window& GetWindow() const
        {
            return *m_Window;
        }

        [[nodiscard]] Renderer::Context& GetRenderContext()
        {
            return m_RenderContext;
        }

        [[nodiscard]] TaskSystem& GetTaskSystem()
        {
            return *m_TaskSystem;
        }

        [[nodiscard]] AssetManager& GetAssetManager()
        {
            return *m_AssetManager;
        }

        // The ImGui layer, or nullptr if the app opted out (ApplicationInfo::ImGui
        // == nullopt).
        [[nodiscard]] ImGuiLayer* GetImGuiLayer() const
        {
            return m_ImGuiLayer.get();
        }

    protected:
        virtual void OnInitialize()
        {
        }

        virtual void OnUpdate(f32 delta)
        {
        }

        virtual void OnRender()
        {
        }

        // Called after the main loop exits and the GPU is idle, immediately
        // before the rendering context is torn down. Release every engine
        // resource held by the application here (reset Refs/Uniques,
        // AssetHandles included) — resources that outlive the context fail on
        // destruction.
        virtual void OnDispose()
        {
        }

        // Ask the run loop to exit after the current frame. This is the only way
        // to stop a headless app (which has no window to close), and also works
        // for windowed apps.
        void RequestExit() { m_ShouldExit = true; }

    private:
        void Initialize();
        void Frame();

        ApplicationInfo m_Info;

        Unique<Window> m_Window;

        Renderer::Context m_RenderContext;

        // The CPU concurrency subsystem, constructed beside the Context and
        // threaded by reference into consumers. Destroyed (joining its workers)
        // after m_AssetManager: a worker may hold a Context& and touch
        // AssetManager state, so neither may be torn down while a worker is live.
        Unique<TaskSystem> m_TaskSystem;

        // Constructed after m_RenderContext (it needs a live Context); reset
        // before DisposeResources() so cached assets retire while the context
        // is still alive.
        Unique<AssetManager> m_AssetManager;

        Unique<ImGuiLayer> m_ImGuiLayer;

        bool m_ShouldExit = false;
    };
}
