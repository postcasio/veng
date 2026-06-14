#include <Veng/Application.h>

#include <Veng/Assert.h>
#include <Veng/Log.h>
#include <Veng/Time.h>

namespace Veng
{
    Application::Application(ApplicationInfo info) :
        m_Info(std::move(info))
    {
    }

    void Application::Initialize()
    {
        if (!m_Info.Headless)
        {
            m_Window = Window::Create(m_Info.WindowInfo);
        }

        m_RenderContext.Initialize({
            .ApplicationName = m_Info.Name,
            .EngineName = m_Info.EngineName,
            .InternalRenderExtent = m_Info.InternalRenderExtent,
        }, m_Window.get());

        m_TaskSystem = CreateUnique<TaskSystem>();

        // The transfer command pools are keyed by worker index, so they can only
        // be created once the worker count is known. Done here, before any upload.
        m_RenderContext.InitializeTransferPools(*m_TaskSystem);

        m_AssetManager = CreateUnique<AssetManager>(m_RenderContext, *m_TaskSystem);

        // ImGui needs a window (GLFW backend), so it's only available windowed.
        if (!m_Info.Headless && m_Info.ImGui)
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

        // Windowed: run until the window closes (or RequestExit). Headless: run
        // until the consumer calls RequestExit().
        while (!m_ShouldExit && (m_Info.Headless || m_Window->IsOpen()))
        {
            Frame();
        }

        m_RenderContext.WaitIdle();

        // Let every in-flight job finish before OnDispose, so continuations that
        // hand resources to the application have all run and there is no live
        // worker touching engine state during teardown.
        m_TaskSystem->WaitForAll();

        // Consumers must release their engine resources here — the context is
        // torn down right after, and resources outliving it are an error.
        OnDispose();

        // Shut ImGui down before the context: its backend, descriptor pool and
        // offscreen target must be released while the device is still alive.
        m_ImGuiLayer.reset();

        // Drop every cached asset (regardless of outstanding AssetHandles) so
        // their engine resources retire into this frame's bins before
        // DisposeResources() drains them.
        m_AssetManager.reset();

        // Join the workers only after the AssetManager is gone: a pending load's
        // worker holds a Context& and touches AssetManager state, so neither may
        // be torn down while a worker is live.
        m_TaskSystem.reset();

        m_RenderContext.DisposeResources();
        m_RenderContext.Dispose();
        m_Window.reset();
    }

    void Application::Frame()
    {
        // Run main-thread continuations before BeginFrame advances the frame
        // index: a continuation that registers a resource or retires a handle
        // must land before AcquireNextFrame, or its GPU-state mutation falls in
        // an ambiguous frame window.
        m_TaskSystem->PumpMainThread();

        // Finalize any async loads whose dependencies are now resident: register
        // into the bindless registry and swap the resource into its cache entry.
        // Main-thread-only, same window as the continuation pump.
        m_AssetManager->PumpFinalizes();

        const f32 delta = Time::Update();

        if (m_ImGuiLayer)
        {
            m_ImGuiLayer->BeginFrame();
        }

        if (m_Window)
        {
            m_Window->Update();
        }

        OnUpdate(delta);

        m_RenderContext.BeginFrame();

        OnRender();

        m_RenderContext.EndFrame();
    }
}
