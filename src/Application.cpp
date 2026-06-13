#include <Veng/Application.h>

#include <Veng/Assert.h>
#include <Veng/Log.h>
#include <Veng/Time.h>
#include <Veng/Renderer/Command.h>

namespace Veng
{
    Application::Application(ApplicationInfo info) :
        m_Info(std::move(info))
    {
    }

    void Application::Initialize()
    {
        m_Window = Window::Create(m_Info.WindowInfo);

        m_RenderContext.Initialize({
            .ApplicationName = m_Info.Name,
            .EngineName = m_Info.EngineName,
            .InternalRenderExtent = m_Info.InternalRenderExtent,
        }, m_Window.get());

        if (m_Info.ImGui)
        {
            m_ImGuiLayer = ImGuiLayer::Create(*m_Info.ImGui, m_RenderContext, *m_Window);
        }

        OnInitialize();
    }

    void Application::Run(vector<string> arguments)
    {
        // If a directory is passed as the first argument, use it as the working directory.
        if (arguments.size() > 1)
        {
            if (std::filesystem::exists(arguments[1]) && std::filesystem::is_directory(arguments[1]))
            {
                std::filesystem::current_path(arguments[1]);
            }
            else
            {
                VE_ASSERT(false, "Invalid directory: {}", arguments[1]);
            }
        }

        Time::Initialize();

        Log::Info("Current Directory: {}", std::filesystem::current_path().string());

        Initialize();

        while (m_Window->IsOpen())
        {
            Frame();
        }

        m_RenderContext.WaitIdle();

        // Consumers must release their engine resources here — the context is
        // torn down right after, and resources outliving it are an error.
        OnDispose();

        // Shut ImGui down before the context: its backend, descriptor pool and
        // offscreen target must be released while the device is still alive.
        m_ImGuiLayer.reset();

        m_RenderContext.DisposeResources();
        m_RenderContext.Dispose();
        m_Window.reset();
    }

    void Application::Frame()
    {
        const f32 delta = Time::Update();

        if (m_ImGuiLayer)
        {
            m_ImGuiLayer->BeginFrame();
        }

        m_Window->Update();
        OnUpdate(delta);

        Renderer::Command::BeginFrame();

        OnRender();

        Renderer::Command::EndFrame();
    }
}
