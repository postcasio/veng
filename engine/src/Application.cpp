#include <Veng/Application.h>

#include <Veng/Assert.h>
#include <Veng/Asset/CookedProject.h>
#include <Veng/Gui/GuiConsumer.h>
#include <Veng/Log.h>
#include <Veng/Time.h>

#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/MaterialInstance.h>
#include <Veng/Renderer/CaptureSurface.h>
#include <Veng/Renderer/CommandBuffer.h>
#include <Veng/Renderer/GatherPass.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/SwapChainCompositePass.h>
#include <Veng/Renderer/Viewport.h>

#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>

#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Transforms.h>
#include <Veng/Scene/SceneSimulation.h>
#include <Veng/Scene/SceneSystem.h>
#include <Veng/Scene/SceneViewport.h>

#include <algorithm>
#include <array>
#include <span>

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

        // Routes the window's events to the consumer registry and the Input snapshot by focus.
        // Borrows the window (nullable headless) and the Input snapshot. The ImGui overlay, when
        // present, registers as the first consumer so it is offered every UI-owned event and reads
        // the cursor-capture signal.
        m_InputRouter = CreateUnique<InputRouter>(m_Window.get(), *m_Input);
        if (m_ImGuiLayer)
        {
            m_InputRouter->RegisterConsumer(*m_ImGuiLayer);
        }

        // The Gui document consumer registers second, behind ImGui: an event ImGui consumes never
        // reaches it, an event it passes is offered to the interactive documents on the engine's
        // viewports. It walks the drive-list (which self-cleans on a viewport's destruction).
        m_GuiConsumer =
            CreateUnique<Gui::GuiConsumer>(*m_InputRouter, *m_Input, m_Window.get(), m_Viewports);
        m_InputRouter->RegisterConsumer(*m_GuiConsumer);

        // The opt-in managed viewport set: Presented viewports owned and driven by the engine so a
        // game pushes only a ViewState (or names a Viewer). Built before OnInitialize so a subclass
        // can Configure one and read its renderer there. The singular ManagedViewport is sugar for a
        // one-element ManagedViewports.
        if (!m_Info.ManagedViewports.empty() || m_Info.ManagedViewport)
        {
            vector<ManagedViewportInfo> infos = m_Info.ManagedViewports;
            if (infos.empty())
            {
                infos.push_back(*m_Info.ManagedViewport);
            }
            BuildManagedViewports(infos);

            // Window-tracking managed viewports follow swapchain resizes so their regions keep
            // tracking the window from their normalized Layouts; SetRegion debounces each
            // SceneRenderer::Resize to the next Render. Headless has no swapchain, so the fixed
            // internal extents stand.
            if (!m_Info.Headless)
            {
                m_RenderContext.AddSwapChainInvalidationCallback(
                    [this] { ResolveManagedViewportLayouts(); });
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

        // The engine-managed game world bootstraps after OnInitialize, so a subclass has already
        // set up its ImGui surface and read the managed viewport. It renders through the managed
        // viewport, so it is gated on one being present.
        if (m_Info.World && GetPrimaryViewport())
        {
            BootstrapWorld();
        }
    }

    void Application::BootstrapWorld()
    {
        // The managed world sets the fixed simulation rate: reconfigure the accumulator to its
        // SimTickRate (60 Hz otherwise) before the first frame drives it.
        m_SimClock = SimClock(SimClockInfo{.TickRate = m_Info.World->SimTickRate});

        // The cooked project names the packs to mount and the startup level; everything resolves
        // beside the executable so the launcher + project + packs move as one directory.
        const path projectFile = ExecutableDirectory() / m_Info.World->Project;
        const Result<CookedProject> project = ReadCookedProject(projectFile);
        VE_ASSERT(project, "{}", project.error());

        for (const string& packName : project->PackMountNames)
        {
            const VoidResult mounted = m_AssetManager->Mount(ExecutableDirectory() / packName);
            VE_ASSERT(mounted, "{}", mounted.error());
        }

        // A --level override selects a different level from the project's mounted packs; without
        // it the cooked project's own startup level is used.
        const AssetId startupLevel = m_LaunchArgs.Level.value_or(project->StartupLevel);
        VE_ASSERT(startupLevel.IsValid(), "project '{}' declares no startup level",
                  m_Info.World->Project.string());

        const AssetResult<AssetHandle<Level>> level = m_AssetManager->LoadSync<Level>(startupLevel);
        VE_ASSERT(level.has_value(), "{}", level.error().Detail);
        m_WorldLevel = *level;

        // Spawn the world first (the scene owns the level's simulation): LoadInto seeds the level's
        // render settings onto a settings entity, so the renderer config is read from the scene by
        // the same TryGetFirst query a system uses, never from the Level asset directly.
        LevelInstance instance = m_WorldLevel.Get()->LoadInto(*m_AssetManager, m_SystemRegistry);
        m_World = std::move(instance.World);

        // The primary world is simulation #0: register it first so the engine ticks it in the drive
        // loop and drives its captures, and SetWorldPaused targets it. It stays registered for the
        // app's life (dropped at teardown, self-unregistering).
        RegisterSimulation(*m_World);

        // Seed the managed viewport's topology and the per-frame view knobs from the scene, starting
        // from the configured initial settings: the level's post knobs (a seeded LevelRenderSettings
        // component). The sky is the scene's Sky component, resolved by the renderer itself each
        // Execute — no consumer seeding. Seeded once here; the game owns later changes.
        Renderer::SceneRendererSettings settings = m_ManagedViewports.front().Info.Settings;
        if (const LevelRenderSettings* render = m_World->TryGetFirst<LevelRenderSettings>())
        {
            ApplyLevelRenderSettings(*render, settings, m_WorldView);
        }
        GetPrimaryViewport()->Configure(settings);

        // Hand the world to the subclass before the simulation starts, so a game can read its
        // config from the scene, wait on residency, or capture input focus first.
        OnWorldLoaded(*m_World, instance.Pending);

        m_World->StartSimulation(
            SystemContext{.Assets = *m_AssetManager, .Input = *m_Input, .Tasks = *m_TaskSystem});
    }

    Renderer::ViewportRegion
    Application::ResolveManagedRegion(const ManagedViewportInfo& info) const
    {
        // A pinned Extent is a fixed render resolution at the origin — the region does not track the
        // window. Otherwise the region is round(Layout · render extent): the swapchain framebuffer
        // extent windowed (larger than the logical window on a HiDPI display), HeadlessExtent headless.
        if (info.Extent != uvec2{})
        {
            return {.Offset = {0, 0}, .Extent = info.Extent};
        }

        const vec2 renderExtent = vec2(m_RenderContext.GetRenderExtent());
        const ivec2 offset = ivec2(glm::round(info.Layout.Offset * renderExtent));
        const uvec2 extent = uvec2(glm::round(info.Layout.Extent * renderExtent));
        return {.Offset = offset, .Extent = extent};
    }

    void Application::BuildManagedViewports(std::span<const ManagedViewportInfo> infos)
    {
        // Drop the prior set first (each Unique self-unregisters from the drive-list), clearing each
        // one's pointer association so no stale pointer lingers in the router. Then build the new set
        // in order so index 0 is the primary.
        for (const ManagedViewport& managed : m_ManagedViewports)
        {
            m_InputRouter->ClearViewportSeat(*managed.Viewport);
        }
        m_ManagedViewports.clear();
        m_ManagedViewports.reserve(infos.size());

        for (const ManagedViewportInfo& info : infos)
        {
            Unique<Renderer::Viewport> viewport = Renderer::Viewport::Create({
                .Context = m_RenderContext,
                .Assets = *m_AssetManager,
                .Region = ResolveManagedRegion(info),
                .ColorFormat = info.ColorFormat,
                .Settings = info.Settings,
                .RenderScale = info.RenderScale,
                .MaxAllocationScale = info.MaxAllocationScale,
                .Role = Renderer::ViewportRole::Presented,
            });

            // Opt-in adaptive resolution: the viewport drives its own per-frame sub-rect scale from
            // GPU frame time over the fixed allocation.
            if (info.DynamicResolution)
            {
                viewport->SetDynamicResolution(*info.DynamicResolution);
            }

            RegisterViewport(*viewport);

            // A managed viewport bound to a seat feeds that seat's pointer input: associate it with
            // the router in the same step it is registered, so a free cursor over its region routes
            // to the seat with no dead frame. An unbound viewport (the default single-camera path)
            // needs no association — under capture the pointer routes to the keyboard seat directly.
            if (info.Viewer != Entity::Null)
            {
                m_InputRouter->AssociateViewportSeat(*viewport, info.Viewer);
            }

            m_ManagedViewports.push_back({.Viewport = std::move(viewport), .Info = info});
        }
    }

    void Application::ResolveManagedViewportLayouts()
    {
        for (ManagedViewport& managed : m_ManagedViewports)
        {
            // A pinned viewport keeps its fixed internal extent; a window-tracking one re-resolves
            // its region from its Layout so the gather places it correctly across the resize.
            if (managed.Info.Extent == uvec2{})
            {
                managed.Viewport->SetRegion(ResolveManagedRegion(managed.Info));
            }
        }
    }

    void Application::PushManagedViewportView(const ManagedViewport& managed, const f32 delta)
    {
        Renderer::Viewport& viewport = *managed.Viewport;

        // Screen-space Gui documents on this viewport lay out in logical points while the region is
        // framebuffer pixels, so feed the window content scale as the UI scale each frame: authored
        // px then render at logical size on a HiDPI display, and re-resolve if the window moves to a
        // differently-scaled monitor. Same GetContentScale() the pointer routing uses, so layout,
        // draw, and hit-testing agree.
        viewport.SetUiScale(m_Window ? m_Window->GetContentScale().x : 1.0f);

        // A viewport with no bound Viewer takes the scene's primary camera — the delivered
        // single-viewport path, byte-identical for the default managed viewport.
        if (managed.Info.Viewer == Entity::Null)
        {
            PushSceneView(viewport, *m_World, m_WorldView, delta, m_SimAlpha);
            return;
        }

        // A bound Viewer resolves that seat's camera at the viewport's aspect, falling back to the
        // default framing when the seat resolves none (mirrors PushSceneView's fallback).
        const Ref<Renderer::ImageView> output = viewport.GetOutput();
        const f32 aspect = static_cast<f32>(output->GetImage()->GetWidth()) /
                           static_cast<f32>(output->GetImage()->GetHeight());

        Renderer::ViewState state = m_WorldView;
        state.World = m_World.get();
        state.Camera = ResolveCameraView(*m_World, managed.Info.Viewer, aspect)
                           .value_or(DefaultCameraView(aspect));
        state.Delta = delta;
        state.Alpha = m_SimAlpha;
        viewport.SetViewState(state);
    }

    namespace
    {
        // The scene-local keyboard/mouse seat a captured pointer routes to: the first
        // (Viewer, SeatInput) with UsesKeyboardMouse, Entity::Null when the scene has none.
        Entity FirstKeyboardSeat(const Scene& scene)
        {
            Entity keyboardSeat = Entity::Null;
            scene.Each<Viewer, SeatInput>(
                [&](const Entity seat, const Viewer&, const SeatInput& devices)
                {
                    if (keyboardSeat == Entity::Null && devices.UsesKeyboardMouse)
                    {
                        keyboardSeat = seat;
                    }
                });
            return keyboardSeat;
        }
    }

    Application::ScopedPointer Application::ComputePointerRouting() const
    {
        if (m_Simulations.empty())
        {
            return {};
        }

        // GLFW reports the cursor in window coordinates (logical points), but the viewport regions
        // hit-tested here live in framebuffer pixels (the swapchain extent), so on a HiDPI display the
        // two differ by the window's content scale. Convert before routing so the pointer lands in the
        // right region and its region-local position matches the window point a picking ray later
        // unprojects.
        const vec2 scale = m_Window ? m_Window->GetContentScale() : vec2{1.0f, 1.0f};
        const ivec2 windowPoint = ivec2(m_Input->GetMousePosition() * scale);
        const bool captured = m_InputRouter->IsGameplayFocused();

        // Identify the viewport that owns the pointer this frame; the routing is scoped to the scene
        // it presents so an Owner handle never leaks into another scene's InputMappingSystem.
        const Renderer::Viewport* owner =
            m_InputRouter->ResolvePointerViewport(windowPoint, captured);

        if (captured)
        {
            // The captured pointer belongs wholly to the cursor seat, in one scene: the cursor seat's
            // viewport's scene, or the primary world when the cursor seat has no viewport (the default
            // single-seat path). Resolve that scene's keyboard seat scene-locally.
            const Scene* scene = owner != nullptr ? owner->GetPresentedScene() : m_World.get();
            if (scene == nullptr)
            {
                return {};
            }
            return {.Routing = {.Owner = FirstKeyboardSeat(*scene), .LocalPosition = {}},
                    .Scene = scene};
        }

        // Free cursor: the pointer routes to the first associated viewport region containing it. No
        // owning viewport means no associated region under the cursor — no sim receives a routing.
        if (owner == nullptr)
        {
            return {};
        }
        return {.Routing = m_InputRouter->ResolvePointer(windowPoint, false, Entity::Null),
                .Scene = owner->GetPresentedScene()};
    }

    SystemContext Application::BuildSystemContext(const Scene& scene, const PointerRouting& pointer,
                                                  const u64 tick, const f32 alpha) const
    {
        SystemContext context{
            .Assets = *m_AssetManager,
            .Input = *m_Input,
            .Tasks = *m_TaskSystem,
            .Pointer = pointer,
            .Tick = tick,
            .Alpha = alpha,
        };

        // Resolve the sim's primary presenting viewport — the first registered Presented viewport
        // whose retained scene is this one — for the view descriptor and debug-draw sink. The
        // retained view is last frame's push (view pushes run after ticks); a never-pushed viewport
        // presents no scene, so it never matches and View stays nullopt.
        for (const Renderer::Viewport* viewport : m_Viewports)
        {
            if (viewport->GetRole() == Renderer::ViewportRole::Presented &&
                viewport->GetPresentedScene() == &scene)
            {
                context.View = SystemViewInfo{
                    .Camera = viewport->GetPresentedCamera(),
                    .Region = viewport->GetRegion(),
                    .UiScale = viewport->GetUiScale(),
                };
                context.Debug = &viewport->GetDebugDraw();
                break;
            }
        }
        return context;
    }

    void Application::ReconfigureManagedViewports(std::span<const ManagedViewportInfo> viewports)
    {
        VE_ASSERT(!m_ManagedViewports.empty(),
                  "ReconfigureManagedViewports requires a managed viewport configured at startup");

        // Defer to the top of the next frame — outside any Scene/viewport-list iteration — mirroring
        // the SetRegion resize debounce. The rebuild constructs/drops viewports, which must not run
        // mid-drive.
        m_PendingReconfigure = vector<ManagedViewportInfo>(viewports.begin(), viewports.end());
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
                // The ImGui layer's invalidation callback (registered earlier, so it ran first)
                // recreated its offscreen image; re-point the composite at it or it samples the
                // retired one (old size → squished, stale content → frozen overlay).
                m_Composite->RefreshImGuiSource();
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

    void Application::RegisterCapture(Renderer::SceneCapture& capture)
    {
        VE_ASSERT(std::ranges::find(m_Captures, &capture) == m_Captures.end(),
                  "SceneCapture is already registered to this Application's drive-list");

        m_Captures.emplace_back(&capture);
        capture.AttachToDriveList(m_Captures);
    }

    void Application::RegisterSimulation(Scene& scene)
    {
        VE_ASSERT(std::ranges::find(m_Simulations, &scene) == m_Simulations.end(),
                  "Scene is already registered to this Application's simulation drive-list");

        m_Simulations.emplace_back(&scene);
        scene.AttachToSimDriveList(m_Simulations);
    }

    SceneSimulation* Application::PrimarySimulation() const
    {
        // Simulation #0 is the primary (the managed world, registered first at bootstrap).
        return m_Simulations.empty() ? nullptr : m_Simulations.front()->GetSimulation();
    }

    void Application::SetWorldPaused(bool paused)
    {
        if (SceneSimulation* primary = PrimarySimulation(); primary != nullptr)
        {
            primary->SetPaused(paused);
        }
    }

    bool Application::IsWorldPaused() const
    {
        const SceneSimulation* primary = PrimarySimulation();
        return primary != nullptr && primary->IsPaused();
    }

    void Application::DriveCaptureSurfaces()
    {
        // Registration gates capture driving, not run-state: iterate every registered scene
        // regardless of started/paused, so a paused primary world (an overlay's PausePrimarySim) still
        // drives its mirrors, and an overlay/offscreen/view-less scene's captures are engine-driven.
        for (Scene* scenePtr : m_Simulations)
        {
            const Scene& world = *scenePtr;
            for (auto [entity, surface] : world.View<Renderer::CaptureSurface>())
            {
                // The capture renders from the entity's world position (a probe centered on it, a
                // mirror placed at it). The surface's material is the sibling MeshRenderer's first.
                const vec3 position = vec3(WorldMatrix(world, entity)[3]);

                MaterialInstance* material = nullptr;
                if (const MeshRenderer* mesh = world.TryGet<MeshRenderer>(entity); mesh != nullptr)
                {
                    if (mesh->Mesh.IsLoaded())
                    {
                        const std::span<const AssetHandle<MaterialInstance>> materials =
                            mesh->Mesh.Get()->GetMaterials();
                        if (!materials.empty() && materials[0].IsLoaded())
                        {
                            material = materials[0].Get();
                        }
                    }
                }

                // Register the capture on the drive-list the first time it materializes; the
                // SceneCapture erases its own pointer on destruction, so removing the
                // component/entity/scene unregisters it with no bookkeeping here.
                const bool hadCapture = surface.GetCapture() != nullptr;
                Renderer::SceneCapture* capture =
                    surface.Drive(m_RenderContext, *m_AssetManager, world, position, material);
                if (capture != nullptr && !hadCapture)
                {
                    RegisterCapture(*capture);
                }
            }
        }
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
        // Parse argv (without the program name) once; the engine consumes the recognised options
        // itself and a game can read them back through GetLaunchArguments().
        const std::span<const string> tokens =
            arguments.empty() ? std::span<const string>{} : std::span(arguments).subspan(1);
        Result<LaunchArguments> parsed = LaunchArguments::Parse(tokens);
        VE_ASSERT(parsed, "{}", parsed.error());
        m_LaunchArgs = std::move(*parsed);

        // A leading positional argument selects the working directory (launcher convention).
        if (m_LaunchArgs.WorkingDirectory)
        {
            const path& dir = *m_LaunchArgs.WorkingDirectory;
            VE_ASSERT(std::filesystem::exists(dir) && std::filesystem::is_directory(dir),
                      "Invalid directory: {}", dir.string());
            std::filesystem::current_path(dir);
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

        // Drop the engine-managed world before the asset manager so its components' AssetHandles
        // (the sky's environment/material, the level handle) retire through the deferred path.
        m_World.reset();
        m_WorldLevel = {};

        // Drop the engine-owned managed tail and managed viewports before the context: the gather
        // and composite hold GPU resources, and each managed viewport self-unregisters from the
        // still-live drive-list. A subclass's panel-owned viewports are released in OnDispose
        // above, so the drive-list is empty (or holds only the managed viewports) by here.
        m_CompositeGraph.reset();
        m_Composite.reset();
        m_GatherGraph.reset();
        m_Gather.reset();

        // The placement cache retains a Ref to each Presented viewport's output view for
        // change-detection; clear it so dropping the viewports below releases their outputs and
        // the images retire, rather than outliving the context's allocator.
        m_GatheredPlacements.clear();
        m_ManagedViewports.clear();

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
        // Apply a deferred managed-viewport reconfigure at the top of the frame, outside any
        // Scene/viewport-list iteration: it drops and constructs viewports (mutating the drive-list),
        // which must not run mid-drive. Regions resolve from each info's Layout here.
        if (m_PendingReconfigure)
        {
            BuildManagedViewports(*m_PendingReconfigure);
            m_PendingReconfigure.reset();
        }

        // Before BeginFrame: continuations that register or retire resources must
        // land before AcquireNextFrame or their GPU-state mutation is frame-ambiguous.
        m_TaskSystem->PumpMainThread();

        // Finalize resident async loads (bindless registration + cache swap) before
        // BeginFrame, in the same main-thread window as the continuation pump.
        m_AssetManager->PumpFinalizes();

        const f32 delta = Time::Update();

        // The fixed-timestep accumulator advances only while a registered simulation is live: a full
        // pause (or no started scene) stops accumulation so resuming chases no backlog. Any active
        // scene drives the shared tick, so an overlay and the primary sim step in phase.
        const bool anyActive =
            std::ranges::any_of(m_Simulations,
                                [](const Scene* scene)
                                {
                                    const SceneSimulation* sim = scene->GetSimulation();
                                    return sim != nullptr && sim->IsStarted() && !sim->IsPaused();
                                });

        SimStep step{};
        if (anyActive)
        {
            step = m_SimClock.Advance(delta);
        }
        else
        {
            m_SimClock.Reset();
        }
        m_SimAlpha = step.Alpha;

        // Roll the input snapshot forward, then poll the window and route this frame's events
        // through the router (folding into the snapshot, forwarding to ImGui by focus).
        // Headless borrows no window, so no events arrive and the snapshot stays neutral. The roll is
        // held only when the previous frame latched (an active sim ran no tick), so a pressed edge on
        // a zero-tick frame survives to the next tick-running frame; a frame with no active sim (the
        // editor, a full pause) rolls every frame like an ordinary UI.
        m_Input->BeginFrame(!m_PreviousFrameLatchedInput);
        if (m_Window)
        {
            m_Window->Update();
            m_Window->DrainEvents([this](Event& event) { m_InputRouter->Dispatch(event); });

            // Poll every joystick slot into the snapshot after BeginFrame's roll: gamepads are
            // polled per frame, unlike the callback-driven keyboard/mouse folded via DrainEvents.
            std::array<GamepadState, 16> pads{};
            m_Window->PollGamepads(pads);
            m_Input->IngestGamepadStates(pads);
        }

        // After the events are forwarded: ImGui's NewFrame consumes them this frame.
        if (m_ImGuiLayer)
        {
            m_ImGuiLayer->BeginFrame();
        }

        // Drive every registered simulation that is started and not paused, in registration order:
        // this frame's fixed Sim steps (0..N, the accumulator's step count, at the shared tick
        // numbers) then one View pass with the interpolation alpha. The pointer routing is scoped to
        // one scene, so each sim gets it only when the pointer's owning viewport presents that sim's
        // scene — a scene-local Owner handle never leaks across.
        const ScopedPointer scoped = ComputePointerRouting();
        for (Scene* scene : m_Simulations)
        {
            const SceneSimulation* sim = scene->GetSimulation();
            if (sim == nullptr || !sim->IsStarted() || sim->IsPaused())
            {
                continue;
            }
            const PointerRouting pointer =
                scene == scoped.Scene ? scoped.Routing : PointerRouting{};
            for (u32 tickIndex = 0; tickIndex < step.Steps; ++tickIndex)
            {
                const u64 tick = step.FirstTick + tickIndex;
                scene->TickSimulationPhase(SceneSystem::Phase::Sim, step.SimDelta,
                                           BuildSystemContext(*scene, pointer, tick, 0.0f));
            }
            scene->TickSimulationPhase(
                SceneSystem::Phase::View, delta,
                BuildSystemContext(*scene, pointer, m_SimClock.GetTick(), step.Alpha));
        }

        // The edge latch: a frame with a live simulation that ran no tick holds its edges for the
        // next tick-running frame; a frame with no active sim never latches (it rolls next frame).
        m_PreviousFrameLatchedInput = anyActive && step.Steps == 0;

        OnUpdate(delta);

        // Push the managed world's render source into each managed viewport: a viewport naming a
        // Viewer gets that seat's camera resolved at its aspect, otherwise the scene's primary
        // camera. Plus the per-frame view knobs. After OnUpdate so a game's per-frame scene edits are
        // reflected; before the viewport render phase reads it.
        if (m_World)
        {
            for (const ManagedViewport& managed : m_ManagedViewports)
            {
                PushManagedViewportView(managed, delta);
            }
        }

        m_RenderContext.BeginFrame();

        Renderer::CommandBuffer& cmd = m_RenderContext.GetCurrentCommandBuffer();

        // Build, register, and push this frame's source into the world's authored capture surfaces,
        // so a scene-declared capture joins the drive-list beside any imperatively-registered ones.
        DriveCaptureSurfaces();

        // The engine render phase, uniform for every app and not overridable. Scene captures
        // render first, so a material sampling a capture's output reads this frame's result
        // during the viewport renders that follow.
        for (Renderer::SceneCapture* capture : m_Captures)
        {
            capture->Render(cmd);
        }

        // Then every registered viewport in registration order (each does its own Execute +
        // Sample barrier), so viewport outputs are in Sample layout before OnRender builds the
        // ImGui draw data that may sample them.
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
