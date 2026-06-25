#include <Veng/Application.h>

#include <Veng/Assert.h>
#include <Veng/Log.h>
#include <Veng/Time.h>

#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/GatherPass.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/SwapChainCompositePass.h>
#include <Veng/Renderer/Viewport.h>

#include <algorithm>

namespace Veng
{
    Application::Application(ApplicationInfo info, TypeRegistry& types, SystemRegistry& systems)
        : m_Info(std::move(info)), m_TypeRegistry(types), m_SystemRegistry(systems)
    {
    }

    void Application::Initialize()
    {
        if (!m_Info.Headless)
        {
            m_Window = Window::Create(m_Info.WindowInfo);
        }

        // Always present: a headless run borrows no window and reports the neutral
        // all-zeros state, so GetInput() and SystemContext::Input are never null.
        m_Input = CreateUnique<Input>(m_Window.get());

        m_RenderContext.Initialize(
            {
                .ApplicationName = m_Info.Name,
                .EngineName = m_Info.EngineName,
                .HeadlessExtent = m_Info.HeadlessExtent,
                .RequestedDisplayMode = m_Info.RequestedDisplayMode,
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

        // Routes the window's events to ImGui and the Input snapshot by focus. Borrows all
        // three; the ImGui layer is nullable (UI-free) and the window is nullable (headless).
        m_InputRouter = CreateUnique<InputRouter>(m_Window.get(), *m_Input, m_ImGuiLayer.get());

        // The opt-in managed primary viewport: one Presented viewport covering the window, owned
        // and driven by the engine so a game pushes only a ViewState. Built before OnInitialize so
        // a subclass can Configure it and read its renderer there.
        if (m_Info.ManagedViewport)
        {
            const ManagedViewportInfo& managed = *m_Info.ManagedViewport;

            // The managed viewport covers the whole window: with no explicit Extent its region
            // is the render-target extent (the swapchain framebuffer extent windowed — larger
            // than the logical window on a HiDPI display — and HeadlessExtent headless), so the
            // gather places it across the full target rather than a sub-rect. A non-zero Extent
            // pins a fixed render resolution instead.
            const bool trackWindow = managed.Extent == uvec2{};
            const uvec2 extent = trackWindow ? m_RenderContext.GetRenderExtent() : managed.Extent;

            m_PrimaryViewport = Renderer::Viewport::Create({
                .Context = m_RenderContext,
                .Assets = *m_AssetManager,
                .Region = {.Offset = {0, 0}, .Extent = extent},
                .ColorFormat = managed.ColorFormat,
                .Settings = managed.Settings,
                .RenderScale = managed.RenderScale,
                .MaxAllocationScale = managed.MaxAllocationScale,
                .Role = Renderer::ViewportRole::Presented,
            });

            // Opt-in adaptive resolution: the viewport drives its own scale from GPU frame time,
            // and (when configured) the allocation tier follows the sustained sub-rect.
            if (managed.DynamicResolution)
            {
                m_PrimaryViewport->SetDynamicResolution(*managed.DynamicResolution,
                                                        managed.AllocationTier);
            }

            RegisterViewport(*m_PrimaryViewport);

            // A window-tracking managed viewport follows swapchain resizes so it keeps covering
            // the window; SetRegion debounces the SceneRenderer::Resize to the next Render.
            // Headless has no swapchain, so the fixed internal extent stands.
            if (trackWindow && !m_Info.Headless)
            {
                m_RenderContext.AddSwapChainInvalidationCallback(
                    [this]
                    {
                        m_PrimaryViewport->SetRegion(
                            {.Offset = {0, 0}, .Extent = m_RenderContext.GetRenderExtent()});
                    });
            }
        }

        // The managed gather + composite tail exists only with ImGui (it feeds the swapchain
        // composite). Headless renders the managed viewport into its offscreen target and the app
        // reads it back directly.
        if (m_ImGuiLayer)
        {
            InitializeManagedTail();
        }

        OnInitialize();
    }

    void Application::InitializeManagedTail()
    {
        m_Gather = Renderer::GatherPass::Create({
            .Context = m_RenderContext,
            .Assets = *m_AssetManager,
            .Extent = m_RenderContext.GetSwapChainExtent(),
        });

        m_Composite = Renderer::SwapChainCompositePass::Create({
            .Context = m_RenderContext,
            .ImGui = *m_ImGuiLayer,
            .Assets = *m_AssetManager,
            .SceneSource = m_Gather->GetOutput(),
            .SwapChainFormat = m_RenderContext.GetSwapChainFormat(),
            .ColorSpace = m_RenderContext.GetActiveDisplayColorSpace(),
        });

        const auto compileGather = [this]
        {
            Renderer::RenderGraph graph(m_RenderContext);
            return m_Gather->Compile(graph);
        };
        const auto compileComposite = [this]
        {
            Renderer::RenderGraph graph(m_RenderContext);
            const Renderer::ResourceId swapId = graph.Import("SwapChain");
            return m_Composite->Compile(graph, swapId);
        };

        // Swapchain recreation invalidates the baked extent and may re-negotiate the surface's
        // format/color space (a window moved to a display with different HDR support); re-target
        // the composite before recompiling.
        m_RenderContext.AddSwapChainInvalidationCallback(
            [this, compileGather, compileComposite]
            {
                m_Gather->Resize(m_RenderContext.GetSwapChainExtent());
                m_Composite->SetSceneSource(m_Gather->GetOutput());
                m_Composite->SetSwapChainTarget(m_RenderContext.GetSwapChainFormat(),
                                                m_RenderContext.GetActiveDisplayColorSpace());
                m_GatherGraph = compileGather();
                m_CompositeGraph = compileComposite();
            });

        m_GatherGraph = compileGather();
        m_CompositeGraph = compileComposite();
    }

    void Application::RegisterViewport(Renderer::Viewport& viewport)
    {
        VE_ASSERT(std::ranges::find(m_Viewports, &viewport) == m_Viewports.end(),
                  "Viewport is already registered to this Application's drive-list");

        m_Viewports.emplace_back(&viewport);
        viewport.AttachToDriveList(m_Viewports);
    }

    void Application::RenderManagedTail(Renderer::CommandBuffer& cmd)
    {
        if (!m_Gather)
        {
            return;
        }

        // Assemble the registered Presented viewports into the gather target, each into its own
        // region. Zero placements composites ImGui over a clear (the editor's case).
        vector<Renderer::CompositePlacement> placements;
        for (const Renderer::Viewport* viewport : m_Viewports)
        {
            if (viewport->GetRole() == Renderer::ViewportRole::Presented)
            {
                placements.emplace_back(Renderer::CompositePlacement{
                    .Texture = viewport->GetOutput(),
                    .Region = viewport->GetRegion(),
                });
            }
        }

        // Rebind only when the placement set changed (output identity or region), so a steady
        // frame issues no bindless re-registration.
        const auto samePlacement =
            [](const Renderer::CompositePlacement& a, const Renderer::CompositePlacement& b)
        {
            return a.Texture == b.Texture && a.Region.Offset == b.Region.Offset &&
                   a.Region.Extent == b.Region.Extent;
        };
        if (!std::ranges::equal(placements, m_GatheredPlacements, samePlacement))
        {
            m_Gather->SetPlacements(placements);
            m_GatheredPlacements = std::move(placements);
        }

        m_Gather->Execute(cmd, *m_GatherGraph);

        // The composite samples the assembly target outside the graph; transition it.
        cmd.PrepareForAccess(m_Gather->GetOutput(), Renderer::AccessKind::Sample);

        m_Composite->Execute(cmd, *m_CompositeGraph,
                             m_RenderContext.GetCurrentSwapChainImageView());
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

        // Drop the engine-owned managed tail and primary viewport before the context: the gather
        // and composite hold GPU resources, and the primary viewport self-unregisters from the
        // still-live drive-list. A subclass's panel-owned viewports are released in OnDispose
        // above, so the drive-list is empty (or holds only the managed primary) by here.
        m_CompositeGraph.reset();
        m_Composite.reset();
        m_GatherGraph.reset();
        m_Gather.reset();

        // The placement cache retains a Ref to each Presented viewport's output view for
        // change-detection; clear it so dropping the viewports below releases their outputs and
        // the images retire, rather than outliving the context's allocator.
        m_GatheredPlacements.clear();
        m_PrimaryViewport.reset();

        // The router borrows the window, input, and ImGui layer; drop it before any of them.
        m_InputRouter.reset();

        // Shut ImGui down before the context: its backend, descriptor pool and
        // offscreen target must be released while the device is still alive.
        m_ImGuiLayer.reset();

        // Drop all cached assets so their GPU resources retire before DisposeResources() drains the bins.
        m_AssetManager.reset();

        // Workers must stop after the AssetManager: a live load worker holds Context& and AssetManager state.
        m_TaskSystem.reset();

        m_RenderContext.DisposeResources();
        m_RenderContext.Dispose();

        // Input borrows the window, so drop it before the window it points at.
        m_Input.reset();
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

        // Roll the input snapshot forward, then poll the window and route this frame's events
        // through the router (folding into the snapshot, forwarding to ImGui by focus).
        // Headless borrows no window, so no events arrive and the snapshot stays neutral.
        m_Input->BeginFrame();
        if (m_Window)
        {
            m_Window->Update();
            m_Window->DrainEvents([this](Event& event) { m_InputRouter->Dispatch(event); });
        }

        // After the events are forwarded: ImGui's NewFrame consumes them this frame.
        if (m_ImGuiLayer)
        {
            m_ImGuiLayer->BeginFrame();
        }

        OnUpdate(delta);

        m_RenderContext.BeginFrame();

        Renderer::CommandBuffer& cmd = m_RenderContext.GetCurrentCommandBuffer();

        // The engine render phase, uniform for every app and not overridable: render every
        // registered viewport in registration order (each does its own Execute + Sample barrier),
        // so viewport outputs are in Sample layout before OnRender builds the ImGui draw data that
        // may sample them.
        for (Renderer::Viewport* viewport : m_Viewports)
        {
            viewport->Render(cmd);
        }

        // The app builds its ImGui frame and records any extra draws; it no longer runs the
        // composite or ImGuiLayer::Render — those bracket it in the engine phase.
        OnRender();

        // When ImGui is on, record the overlay then composite the Presented viewports behind it.
        if (m_ImGuiLayer)
        {
            m_ImGuiLayer->Render(cmd);
            RenderManagedTail(cmd);
        }

        m_RenderContext.EndFrame();
    }
}
