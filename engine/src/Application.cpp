#include <Veng/Application.h>

#include <Veng/Assert.h>
#include <Veng/Log.h>
#include <Veng/Time.h>

namespace Veng
{
    Application::Application(ApplicationInfo info, TypeRegistry& types)
        : m_Info(std::move(info)), m_TypeRegistry(types)
    {
    }

    void Application::Initialize()
    {
        if (!m_Info.Headless)
        {
            m_Window = Window::Create(m_Info.WindowInfo);
        }

        m_RenderContext.Initialize(
            {
                .ApplicationName = m_Info.Name,
                .EngineName = m_Info.EngineName,
                .InternalRenderExtent = m_Info.InternalRenderExtent,
                .PipelineCachePath = m_Info.PipelineCachePath,
            },
            m_Window.get());

        m_TaskSystem = CreateUnique<TaskSystem>();

        // The transfer command pools are keyed by worker index, so they can only
        // be created once the worker count is known. Done here, before any upload.
        m_RenderContext.InitializeTransferPools(*m_TaskSystem);

        m_AssetManager = CreateUnique<AssetManager>(m_RenderContext, *m_TaskSystem, m_TypeRegistry);

        // ImGui needs a window (GLFW backend), so it's only available windowed.
        if (!m_Info.Headless && m_Info.ImGui)
        {
            m_ImGuiLayer = ImGuiLayer::Create(*m_Info.ImGui, m_RenderContext, *m_Window);
        }

        OnInitialize();
    }

    void Application::Run(vector<string> arguments)
    {
        // A second argument selects the working directory (launcher convention).
        if (arguments.size() > 1)
        {
            if (std::filesystem::exists(arguments[1]) &&
                std::filesystem::is_directory(arguments[1]))
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

        // Drain in-flight jobs before OnDispose: continuations that hand resources
        // back to the app must complete before teardown touches engine state.
        m_TaskSystem->WaitForAll();

        OnDispose();

        // Shut ImGui down before the context: its backend, descriptor pool and
        // offscreen target must be released while the device is still alive.
        m_ImGuiLayer.reset();

        // Drop all cached assets so their GPU resources retire before DisposeResources() drains the bins.
        m_AssetManager.reset();

        // Workers must stop after the AssetManager: a live load worker holds Context& and AssetManager state.
        m_TaskSystem.reset();

        m_RenderContext.DisposeResources();
        m_RenderContext.Dispose();
        m_Window.reset();
    }

    void Application::Frame()
    {
        // Before BeginFrame: continuations that register or retire resources must
        // land before AcquireNextFrame or their GPU-state mutation is frame-ambiguous.
        m_TaskSystem->PumpMainThread();

        // Finalize resident async loads (bindless registration + cache swap) before
        // BeginFrame, in the same main-thread window as the continuation pump.
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
