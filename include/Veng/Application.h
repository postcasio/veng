#pragma once

#include <Veng/Veng.h>
#include <Veng/Window.h>
#include <Veng/Renderer/Backend/Context.h>

namespace Veng
{
    struct ApplicationInfo
    {
        string Name = "Veng Application";
        string EngineName = "Veng";
        uvec2 InternalRenderExtent{1280, 720};
        WindowInfo WindowInfo;
        optional<path> DefaultFontPath;
        optional<path> IconFontPath;
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

    private:
        void Initialize();
        void Frame();

        ApplicationInfo m_Info;

        Unique<Window> m_Window;

        Renderer::Context m_RenderContext;
    };
}
