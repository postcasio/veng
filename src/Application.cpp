#include <Veng/Application.h>

#include <Veng/Log.h>
#include <Veng/Time.h>
#include <Veng/Renderer/Backend/Command.h>

namespace Veng
{
    Application::Application(ApplicationInfo info) :
        m_Info(std::move(info))
    {
    }

    void Application::Initialize()
    {
        m_Window = m_RenderContext.Initialize({
            .ApplicationName = m_Info.Name,
            .EngineName = m_Info.EngineName,
            .InternalRenderExtent = m_Info.InternalRenderExtent,
            .WindowInfo = m_Info.WindowInfo,
            .DefaultFontPath = m_Info.DefaultFontPath,
            .IconFontPath = m_Info.IconFontPath
        });

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
                throw std::runtime_error(fmt::format("Invalid directory: {}", arguments[1]));
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

        OnDispose();
    }

    void Application::Frame()
    {
        const f32 delta = Time::Update();

        m_RenderContext.BeginFrame();

        m_Window->Update();
        OnUpdate(delta);

        Renderer::Command::BeginFrame();

        OnRender();

        Renderer::Command::EndFrame();
    }
}
