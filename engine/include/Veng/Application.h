#pragma once

#include <Veng/Veng.h>
#include <Veng/Window.h>
#include <Veng/Renderer/Context.h>
#include <Veng/ImGui/ImGuiLayer.h>

namespace Veng
{
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
        // resource held by the application here (reset Refs/Uniques) —
        // resources that outlive the context fail on destruction.
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

        Unique<ImGuiLayer> m_ImGuiLayer;

        bool m_ShouldExit = false;
    };
}
