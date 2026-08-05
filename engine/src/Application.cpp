#include <Veng/Application.h>

#include <Veng/Assert.h>
#include <Veng/Asset/CookedProject.h>
#include <Veng/Audio/AudioDevice.h>
#include <Veng/Diagnostics/Profiler.h>
#include <Veng/Gui/GuiConsumer.h>
#include <Veng/Log.h>
#include <Veng/Platform/UserPaths.h>
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

#include <Veng/Asset/Level.h>
#include <Veng/Asset/Prefab.h>

#include <Veng/Net/Client.h>
#include <Veng/Net/Host.h>
#include <Veng/Net/InputFeed.h>
#include <Veng/Net/WorldEnvelope.h>
#include <Veng/WorldRunner.h>

#include <Veng/Scene/Camera.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/InputMappingSystem.h>
#include <Veng/Scene/LocalControl.h>
#include <Veng/Scene/Requests.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/Transforms.h>
#include <Veng/Scene/SceneSimulation.h>
#include <Veng/Scene/SceneSystem.h>
#include <Veng/Scene/SceneViewport.h>

#include "Scene/FocusRequestReconcile.h"
#include "Scene/RequestDrain.h"

#include <fmt/format.h>

#include <algorithm>
#include <array>
#include <cmath>
#include <cstdlib>
#include <deque>
#include <span>
#include <unordered_map>

namespace Veng
{
    /// @brief The mounted net hosts and the input buffers threaded around the world drive.
    ///
    /// One of the two arms is live per net launch mode: Server (the ServerHost + a per-connection input
    /// jitter buffer) for `--server`, or Client (the owned Net::Client + ClientHost + the input send
    /// window) for `--join`. Role reports which transport arm the process mounts — its transport
    /// capability — while WorldRoles carries the orthogonal per-world authority axis.
    struct Application::NetState
    {
        NetRole Role = NetRole::Server;
        GameNetInfo Info;

        // The host-side per-world authority role, keyed by WorldInstanceId value: which NetRole each
        // net-active world ticks under. The transport binds one world here, so it holds that world
        // alone; the world drive stamps a world's per-tick context Role from it and pumps net only for a
        // world it names. A world absent from the map is Server-tier with no transport (standalone).
        unordered_map<u64, NetRole> WorldRoles;

        // Server arm.
        Unique<ServerHost> Server;
        // The per-(connection, joined world) input jitter buffers, keyed by InputBufferKey — a
        // connection multiplexes N worlds, each its own input stream.
        unordered_map<u64, InputJitterBuffer> Jitter;

        // Client arm.
        Unique<Net::Client> Client;
        Unique<ClientHost> ClientHost;

        // Per client-joined world: its input send window, retained level, spawn residency, and the
        // clock-seed/slew latches driving its Client-tier sim. Keyed by WorldInstanceId value. The
        // single-join session has one entry (the managed world); a multiplexed client one per join.
        struct ClientWorldState
        {
            InputSendBuffer Send;
            // Keeps the joined level resident for the world's lifetime (its spawned components reference it).
            AssetHandle<Level> Level;
            // The spawn residency, held from the join load until the world's simulation starts.
            ResidencyBatch Pending;
            bool Started = false;
            // True once this world's SimClock has been seeded to the server's tick (its first snapshot
            // reveals it). Until then its tick epoch is unrelated to the server's.
            bool ClockSeeded = false;
            // This world's sim time-scale this frame (the client slew toward its target tick lead); 1
            // when not yet syncing. Applied as the world's SimScale so it runs its sim ahead of the server.
            f32 Slew = 1.0f;
        };
        unordered_map<u64, ClientWorldState> ClientWorlds;
        // The runner worlds each in-flight join installs into, in join-request (FIFO) order: LoadLevel
        // pops the front to place the reply's scene, so a reply lands in the world its join opened.
        std::deque<WorldInstanceId> PendingJoinWorlds;
        // The runner world each granted JoinId's scene lives in, rebuilt each PumpNet — so a directed
        // travel's leave can close the departed join's world after the ClientHost has dropped the join.
        unordered_map<u64, WorldInstanceId> JoinWorlds;
        // The managed viewport a client-arm presenting travel targets, keyed by the destination
        // WorldKey; consumed when the key's join installs. A presenting join with no entry — an
        // unprompted directed travel, a reattach's gameplay restore — presents on viewport 0.
        std::unordered_map<Net::WorldKey, usize> PresentTargets;

        // Prefabs a replicated spawn resolved, kept resident so their entities instantiate; keyed by
        // AssetId value.
        unordered_map<u64, AssetHandle<Prefab>> ClientPrefabs;
    };

    Application::Application(ApplicationInfo info, TypeRegistry& types, SystemRegistry& systems)
        : m_Info(std::move(info)), m_TypeRegistry(types), m_SystemRegistry(systems),
          m_Compositor(m_RenderContext)
    {
    }

    Application::~Application() = default;

    NetRole Application::GetNetRole() const
    {
        return m_Net ? m_Net->Role : NetRole::Server;
    }

    ServerHost* Application::GetServerHost() const
    {
        return m_Net ? m_Net->Server.get() : nullptr;
    }

    ClientHost* Application::GetClientHost() const
    {
        return m_Net ? m_Net->ClientHost.get() : nullptr;
    }

    WorldDirectory* Application::GetWorldDirectory() const
    {
        return m_Directory.get();
    }

    NetRole Application::RoleForWorld(const WorldInstanceId world) const
    {
        if (m_Net)
        {
            const auto it = m_Net->WorldRoles.find(world.Value);
            if (it != m_Net->WorldRoles.end())
            {
                return it->second;
            }
        }
        return NetRole::Server;
    }

    bool Application::IsWorldNetActive(const WorldInstanceId world) const
    {
        if (!m_Net)
        {
            return false;
        }
        if (m_Net->WorldRoles.contains(world.Value))
        {
            return true;
        }
        // On a hosting process every live directory bucket is join-visible (a standalone travel's
        // world converges with a remote join), so its stream rides the mounted host: change ticks
        // stamp and wire input feeds for it like any hosted world.
        return m_Net->Server != nullptr && m_Directory && m_Directory->Contains(world);
    }

    u64 Application::WorldSimTick(const WorldInstanceId world) const
    {
        const World* resolved =
            m_WorldRunner != nullptr ? m_WorldRunner->ResolveWorld(world) : nullptr;
        return resolved != nullptr ? resolved->Clock.GetTick() : 0;
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

        m_AssetManager = CreateUnique<AssetManager>(
            m_RenderContext, *m_TaskSystem, m_TypeRegistry,
            AssetManagerInfo{.AssetTypes = m_Info.AssetTypes, .Loaders = m_Info.AssetLoaders});

        // The audio subsystem. A headless run (CI, a dedicated server, the cooker) takes the null
        // backend; a windowed run tries the hardware device and falls to null when none initializes.
        // The self-test tone is gated behind an environment flag so it never sounds on an ordinary
        // launch.
        m_AudioDevice = Audio::AudioDevice::Create(Audio::AudioDeviceInfo{
            .Backend = m_Info.Headless ? Audio::AudioBackend::Null : Audio::AudioBackend::Auto,
            .RunSelfTest = std::getenv("VENG_AUDIO_SELFTEST") != nullptr,
        });

        // The sim-domain scheduler owning every open world. Given the live device services, so it can
        // spawn cooked-level worlds and drive their capture surfaces.
        m_WorldRunner = CreateUnique<WorldRunner>(WorldRunnerInfo{
            .Types = &m_TypeRegistry,
            .Systems = &m_SystemRegistry,
            .Assets = m_AssetManager.get(),
            .Context = &m_RenderContext,
        });

        // The one place CloseWorld gets a stop context, covering every world however its simulation
        // was started (at open, or externally through StartWorldScene). Built over the same services a
        // world's ticks saw, so each system's OnStop releases what its OnStart acquired. The services
        // are all live by now, so the guard is defensive; a window without them yields nullopt and the
        // world closes without running OnStop rather than dereferencing a missing service.
        m_WorldRunner->SetStopContextFactory(
            [this](const WorldInstanceId world, Scene&) -> optional<SystemContext>
            {
                if (m_AssetManager == nullptr || m_Input == nullptr || m_TaskSystem == nullptr ||
                    m_AudioDevice == nullptr)
                {
                    return std::nullopt;
                }
                return MakeWorldContext(world);
            });

        // ImGui needs a window (GLFW backend), so it's only available windowed.
        if (!m_Info.Headless && m_Info.ImGui)
        {
            // The layout file's default is resolved here rather than in the layer, because the
            // application name that keys the per-user configuration directory lives on this info
            // and nowhere else. An unresolvable directory leaves the path unset, which the layer
            // reads as "persist no layout" — the safe reading, since the alternative is ImGui's
            // own working-directory-relative default.
            ImGuiLayerInfo imguiInfo = *m_Info.ImGui;
            if (!imguiInfo.IniPath)
            {
                if (const Result<path> configDir = UserConfigDir(m_Info.Name))
                {
                    imguiInfo.IniPath = *configDir / "imgui.ini";
                }
                else
                {
                    Log::Warn("imgui: no writable configuration directory ({}); the UI layout "
                              "will not persist across runs",
                              configDir.error());
                }
            }
            m_ImGuiLayer = ImGuiLayer::Create(imguiInfo, m_RenderContext, *m_Window);
        }

        // Routes the window's events to the consumer registry and the Input snapshot by focus.
        // Borrows the window (nullable headless) and the Input snapshot. The ImGui overlay, when
        // present, registers as the first consumer so it is offered every UI-owned event and reads
        // the cursor-capture signal.
        m_InputRouter = CreateUnique<InputRouter>(m_Window.get(), *m_Input,
                                                  m_RenderContext.GetViewportRegistry());
        m_InputRouter->SetBackgroundInput(m_LaunchArgs.BackgroundInput);
        if (m_ImGuiLayer)
        {
            m_InputRouter->RegisterConsumer(*m_ImGuiLayer);
        }

        // The Gui document consumer registers second, behind ImGui: an event ImGui consumes never
        // reaches it, an event it passes is offered to the interactive documents on the engine's
        // viewports. It walks the drive-list (which self-cleans on a viewport's destruction).
        m_GuiConsumer = CreateUnique<Gui::GuiConsumer>(*m_InputRouter, *m_Input, m_Window.get(),
                                                       m_Compositor.GetViewports());
        m_InputRouter->RegisterConsumer(*m_GuiConsumer);

        // The managed-viewport policy collaborator, over the compositor + router. Presentation-only:
        // it owns the Presented viewports the engine drives and pulls their cameras from the runner.
        m_ManagedViewports = CreateUnique<ManagedViewportSet>(
            m_RenderContext, *m_AssetManager, m_Compositor, *m_InputRouter, m_GuiDriverRegistry);

        // The opt-in managed viewport set: Presented viewports owned and driven by the engine so a
        // game pushes only a ViewState (or names a World/Viewer). Built before OnInitialize so a
        // subclass can Configure one and read its renderer there. The singular ManagedViewport is
        // sugar for a one-element ManagedViewports.
        if (!m_Info.ManagedViewports.empty() || m_Info.ManagedViewport)
        {
            vector<ManagedViewportInfo> infos = m_Info.ManagedViewports;
            if (infos.empty())
            {
                infos.push_back(*m_Info.ManagedViewport);
            }
            m_ManagedViewports->Build(infos);
        }

        // Every window-tracking viewport (a managed one, or an overlay registered at runtime) follows
        // swapchain resizes so its region and UI scale keep tracking the window from its normalized
        // Layout; the compositor re-resolves each Layout-carrying viewport it drives and SetRegion
        // debounces the SceneRenderer::Resize to the next Render. Headless has no swapchain, so the
        // fixed internal extents stand.
        if (!m_Info.Headless)
        {
            m_RenderContext.AddSwapChainInvalidationCallback(
                [this] { m_Compositor.ResolveTrackingLayouts(); });
        }

        // The managed gather + composite tail exists only with ImGui (it feeds the swapchain
        // composite). Headless renders the managed viewport into its offscreen target and the app
        // reads it back directly.
        if (m_ImGuiLayer)
        {
            m_Compositor.InitializeTail(*m_AssetManager, *m_ImGuiLayer);
        }

        OnInitialize();

        // A subclass that hit a fatal startup failure calls RequestExit(status) from OnInitialize;
        // initialization stops here rather than bootstrapping a world the app has already given up
        // on, and Run skips the loop entirely. Teardown still runs in full.
        if (m_ShouldExit)
        {
            return;
        }

        // The engine-managed game world bootstraps after OnInitialize, so a subclass has already
        // set up its ImGui surface and read the managed viewport. It renders through the managed
        // viewport, so it is gated on one being present.
        if (m_Info.World && m_ManagedViewports->Get(0))
        {
            BootstrapWorld();
        }
    }

    void Application::BootstrapWorld()
    {
        // Resolve the local player's account once per activation: the Identity hook, or a
        // process-random ephemeral id when unset (valid, but keyed to nothing durable). A headless
        // dedicated launch resolves none — the host is nobody, so no local account joins anything.
        if (!(m_Info.Headless && m_LaunchArgs.Server))
        {
            const GameNetInfo net = m_Info.Net.value_or(GameNetInfo{});
            m_LocalAccount = net.Identity ? net.Identity() : Net::GenerateAccountId();
            VE_ASSERT(m_LocalAccount.IsValid(),
                      "GameNetInfo::Identity returned the invalid account id");
            // The account's opaque profile resolves beside its id, so the local player presents one
            // value whether the process connects, listens, or runs standalone.
            m_LocalProfile = net.PresentProfile ? net.PresentProfile() : Net::Blob{};
        }

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

        // Client mode opens world #0 as an empty join target: the level comes from the accept payload
        // and loads into this world's scene when the accept lands (PumpNet → StartWorldScene). The
        // world is opened (bound to the managed viewport) but not started until the join loads it.
        if (m_LaunchArgs.Join)
        {
            m_ManagedWorld = m_WorldRunner->OpenWorld(WorldOpenInfo{
                .SimTickRate = m_Info.World->SimTickRate,
                .StartSimulation = false,
            });
            // Bind managed viewport #0 to world #0: the per-frame pull presents this world's scene
            // through the primary viewport once the join loads it.
            m_ManagedViewports->SetViewportWorld(0, m_ManagedWorld);
            // The world directory runs in every role; a joining client still owns one (its managed
            // world is a pinned bucket, and a later Travel resolves through it).
            BuildWorldDirectory();
            const JoinTarget& target = *m_LaunchArgs.Join;
            const u16 port =
                target.Port != 0 ? target.Port : m_Info.Net.value_or(GameNetInfo{}).Port;
            const VoidResult connected = ConnectClient(target.Host, port);
            VE_ASSERT(connected, "{}", connected.error());
            return;
        }

        // Standalone / server: open world #0 spawning the startup level (the scene owns the level's
        // simulation). LoadInto seeds the level's render settings onto a settings entity, so the
        // renderer config is read from the scene by the same TryGetFirst query a system uses, never
        // from the Level asset directly.
        const AssetResult<AssetHandle<Level>> level = m_AssetManager->LoadSync<Level>(startupLevel);
        VE_ASSERT(level.has_value(), "{}", level.error().Detail);
        m_WorldLevel = *level;

        m_ManagedWorld = m_WorldRunner->OpenWorld(WorldOpenInfo{
            .Source = m_WorldLevel,
            .SimTickRate = m_Info.World->SimTickRate,
            .StartSimulation = true,
            // Seed the viewport and hand the world to the subclass before the simulation starts, so a
            // game can read its config from the scene, wait on residency, or capture input focus.
            .OnLoaded =
                [this](const WorldInstanceId world, Scene& scene, ResidencyBatch& pending)
            {
                SeedViewportFromWorld(scene);
                OnWorldLoaded(world, scene, pending);
            },
            // The standalone/server bootstrap opens the managed world Server-tier: it owns and advances
            // authoritative state whether or not a transport is later bound (`--server`).
            .MakeStartContext =
                [this]
            {
                return SystemContext{.Assets = *m_AssetManager,
                                     .Input = *m_Input,
                                     .Tasks = *m_TaskSystem,
                                     .Audio = m_AudioDevice->GetEngine(),
                                     .Role = NetRole::Server};
            },
        });

        // Bind managed viewport #0 to world #0: the per-frame pull presents this world's scene and
        // resolves its camera through the primary viewport.
        m_ManagedViewports->SetViewportWorld(0, m_ManagedWorld);

        // The role-neutral world directory: the managed world is a pre-registered (never-reaped) bucket,
        // and Travel / a mounted host resolve through it. The presentation pins follow at the frame's
        // rebind apply point, so the managed world is pinned by viewport #0 from the first frame.
        BuildWorldDirectory();

        // Standalone continue: the local account's record restores through the same registry and
        // directory a reconnect reattaches through — no wire, one code path. Opted out, world #0
        // stays presented and the game calls RestoreLocalSession itself once its store is open.
        if (m_Info.World->RestoreLocalSessionOnBoot)
        {
            RestoreLocalSession();
        }

        // `--server` opens the host on the just-started world: it accepts connections, spawns a seat
        // per connection, and streams state. A game that set no ApplicationInfo::Net still hosts on the
        // zero-config defaults here.
        if (m_LaunchArgs.Server)
        {
            const VoidResult hosting = StartServer(startupLevel);
            VE_ASSERT(hosting, "{}", hosting.error());
        }
    }

    void Application::RestoreLocalSession()
    {
        if (!m_Sessions || !m_Directory || !m_LocalAccount.IsValid())
        {
            return;
        }
        m_Sessions->EnsureLoaded(m_LocalAccount);
        const optional<Net::SessionRecord> record = m_Sessions->BeginReattach(m_LocalAccount);
        if (!record.has_value())
        {
            return;
        }

        // Standing joins warm their worlds under a local pin — standalone presence is a pin, there
        // being no connection to report a join. A failed resolve skips the entry and keeps the rest.
        for (const Net::WorldKey& key : record->StandingJoins)
        {
            const WorldResolveResult resolve =
                m_Directory->Resolve(Net::JoinRequestInfo{.Connection = Net::ConnectionId{},
                                                          .Account = m_LocalAccount,
                                                          .Key = key,
                                                          .Payload = Net::Blob{},
                                                          .Profile = GetLocalProfile()},
                                     /*heldWorlds=*/0);
            if (resolve.Outcome == WorldResolveOutcome::Denied)
            {
                Log::Warn("standalone continue: standing join {:016X}{:016X} did not resolve "
                          "(reason {}); skipping it",
                          key.Hi, key.Lo, static_cast<u32>(resolve.Reason));
                continue;
            }
            AcquireLocalStanding(key, resolve.World);
        }

        if (record->Gameplay.Key == Net::WorldKey{})
        {
            return;
        }
        const WorldResolveResult resolve =
            m_Directory->Resolve(Net::JoinRequestInfo{.Connection = Net::ConnectionId{},
                                                      .Account = m_LocalAccount,
                                                      .Key = record->Gameplay.Key,
                                                      .Payload = record->Gameplay.Params,
                                                      .Profile = GetLocalProfile()},
                                 /*heldWorlds=*/0);
        if (resolve.Outcome == WorldResolveOutcome::Denied)
        {
            // The record is kept, not cleared: a standalone resolve failure is often transient (a
            // missing pack, a factory hiccup), and persisting a cleared record would destroy the
            // continue permanently on one bad run. The currently presented world stays up and the
            // next restore retries the same record.
            Log::Warn("standalone continue: the gameplay world did not resolve (reason {}); "
                      "leaving the current world presented, record kept for the next restore",
                      static_cast<u32>(resolve.Reason));
            return;
        }
        if (m_ManagedViewports->GetCount() > 0)
        {
            m_ManagedViewports->RebindWorldWhenReady(0, resolve.World);
        }
    }

    void Application::ReleaseLocalSession()
    {
        // The standing pins are the process's hold on the restored worlds, not the account's
        // memberships: dropped without touching the record's standing list, so the record persists
        // intact and the worlds' dwells own their fate once no other presence remains.
        const f64 now = static_cast<f64>(Time::Now());
        for (const auto& [key, world] : m_LocalStandingWorlds)
        {
            if (m_Directory)
            {
                m_Directory->Unpin(world, now, m_LocalAccount);
            }
        }
        m_LocalStandingWorlds.clear();

        if (m_Sessions && m_LocalAccount.IsValid())
        {
            m_Sessions->Evict(m_LocalAccount);
        }
    }

    void Application::SeedViewportFromWorld(Scene& world)
    {
        // Seed the managed viewport's topology and the per-frame view knobs from the scene, starting
        // from the configured initial settings: the level's post knobs (a seeded LevelRenderSettings
        // component). The sky is the scene's Sky component, resolved by the renderer itself each
        // Execute — no consumer seeding. Seeded once; the game owns later changes.
        Renderer::Viewport* primary = m_ManagedViewports->Get(0);
        Renderer::SceneRendererSettings settings = primary->GetSettings();
        if (const LevelRenderSettings* render = world.TryGetFirst<LevelRenderSettings>())
        {
            ApplyLevelRenderSettings(*render, settings, m_WorldView);
        }
        primary->Configure(settings);
    }

    VoidResult Application::StartServer(const AssetId levelId)
    {
        const GameNetInfo net = m_Info.Net.value_or(GameNetInfo{});

        m_Net = CreateUnique<NetState>();
        m_Net->Role = NetRole::Server;
        m_Net->Info = net;
        // The hosted managed world ticks Server-tier — its Sim owns and advances authoritative state.
        m_Net->WorldRoles[m_ManagedWorld.Value] = NetRole::Server;

        Result<Unique<ServerHost>> host = ServerHost::Create(ServerHostInfo{
            .Server = Net::ServerInfo{.Port = net.Port,
                                      .MaxConnections = net.MaxConnections,
                                      // The game's account admission (verify/normalize/refuse); the
                                      // unset default accepts as presented (the LAN-trust posture).
                                      .AdmitAccount = net.AdmitAccount,
                                      .NetSim = m_LaunchArgs.NetSim},
            .WorldId = m_ManagedWorld,
            .World = m_WorldRunner->ResolveWorld(m_ManagedWorld)->GetScene(),
            .Assets = *m_AssetManager,
            .LevelId = levelId,
            // Echoed in each join reply so a joining client's tick-offset estimator runs at the
            // hosted world's actual rate.
            .SimTickRate = m_Info.World->SimTickRate,
            .Replication =
                ReplicationServer::Settings{
                    .SnapshotInterval = net.SnapshotIntervalTicks,
                    .QuantizeSpatial = net.QuantizeSpatial,
                    .Quantization =
                        Net::QuantizationSettings{.PositionQuantum = net.PositionQuantum,
                                                  .PositionExtent = net.PositionExtent,
                                                  .RotationBits = net.RotationBits},
                    .KeyframeInterval = net.KeyframeIntervalSnapshots},
            .Interest = Net::InterestSettings{.Radius = net.InterestRadius,
                                              .LeaveMultiplier = net.InterestLeaveMultiplier,
                                              .MinDwellSnapshots = net.InterestMinDwellSnapshots},
            .InterestPolicy = net.InterestPolicy,
            .MaxJoinedWorldsPerConnection = net.MaxJoinedWorldsPerConnection,
            .MaxHostedWorlds = net.MaxHostedWorlds,
            .MaxPlayersPerInstance = net.MaxPlayersPerInstance,
            .IdleKeepWarmDwell = net.IdleKeepWarmDwell,
            // The listen host's own player is an account-addressed message recipient (loopback,
            // connection-free); a dedicated host resolves no local account and this stays invalid.
            .LocalAccount = m_LocalAccount,
            // The listen host's own player presents no connect request, so its profile is bound
            // here — ProfileOf and the local join arm then answer as they do for a connected account.
            .LocalProfile = m_LocalProfile,
            // The host consumes the Application-owned directory (get-or-place, presence, dwell, reap)
            // and session registry (records, reattach, durability); the policy hooks and caps
            // already live in them, so they are not repeated here.
            .Directory = m_Directory.get(),
            .Sessions = m_Sessions.get(),
        });
        if (!host)
        {
            // A failed bind leaves no net mode active, so a caller may retry (or fall back to standalone).
            m_Net.reset();
            return std::unexpected(fmt::format("server host failed to open: {}", host.error()));
        }
        m_Net->Server = std::move(*host);

        const Result<u16> port = m_Net->Server->Server().LocalPort();
        Log::Info("Listening on port {}", port.value_or(net.Port));
        return {};
    }

    VoidResult Application::ConnectClient(const string& host, const u16 port)
    {
        const GameNetInfo net = m_Info.Net.value_or(GameNetInfo{});

        m_Net = CreateUnique<NetState>();
        m_Net->Role = NetRole::Client;
        m_Net->Info = net;

        // The auto-join binds the managed world #0 as the Client-tier join target: its Sim displays
        // replicated state and advances only its client-local (and predicted) entities, and the fixed
        // DefaultWorldKey reply loads into it (queued FIFO like any join). With the auto-join off, the
        // managed world is left as it is — a consumer joins the worlds it wants through JoinWorld, each
        // into its own runner world.
        if (net.AutoJoinDefaultWorld)
        {
            m_Net->WorldRoles[m_ManagedWorld.Value] = NetRole::Client;
            m_Net->ClientWorlds[m_ManagedWorld.Value].Send =
                InputSendBuffer(InputSendBuffer::Settings{.Redundancy = net.InputRedundancyTicks});
            m_Net->PendingJoinWorlds.push_back(m_ManagedWorld);
        }

        // The local account (the Identity hook's result, resolved at bootstrap) is what this
        // connection presents at the handshake; the server binds every seat and join to it.
        Result<Unique<Net::Client>> client = Net::Client::Connect(Net::ClientInfo{
            .Host = host,
            .Port = port,
            .Account = m_LocalAccount,
            .Profile = m_LocalProfile,
            .NetSim = m_LaunchArgs.NetSim,
        });
        if (!client)
        {
            // A failed connect leaves no net mode active, so the caller stays standalone (or retries).
            m_Net.reset();
            return std::unexpected(
                fmt::format("client failed to connect to {}:{}: {}", host, port, client.error()));
        }
        m_Net->Client = std::move(*client);

        m_Net->ClientHost = ClientHost::Create(ClientHostInfo{
            .Client = *m_Net->Client,
            .Assets = *m_AssetManager,
            // Auto-join the DefaultWorldKey into the managed world on connect unless opted out, in which
            // case the consumer drives every join through JoinWorld.
            .AutoJoin = net.AutoJoinDefaultWorld,
            // The client mirror of the server's factory-supplied digest: yields the digest this client
            // expects for the key it joins, validated against the reply's echo. Unset (the default)
            // presents the zero digest.
            .WorldDigest = net.ClientWorldDigest,
            .LoadLevel = [this](const AssetId id) -> Scene* { return LoadClientLevel(id); },
            .OpenEmptyWorld = [this]() -> Scene* { return OpenEmptyClientWorld(); },
            .ResolvePrefab = [this](const AssetId id) -> Ref<Prefab>
            {
                const AssetResult<AssetHandle<Prefab>> prefab =
                    m_AssetManager->LoadSync<Prefab>(id);
                if (!prefab.has_value())
                {
                    return nullptr;
                }
                // Keep the handle resident (the spawn instantiates from it), then hand a non-owning
                // Ref aliasing the resident Prefab — the replication client spawns from it inline.
                const AssetHandle<Prefab>& held =
                    m_Net->ClientPrefabs.try_emplace(id.Value, *prefab).first->second;
                return Ref<Prefab>(std::shared_ptr<void>{}, held.Get());
            },
            .OnPossession = [this](Scene& world, const Entity pawn)
            { NotifyPossession(world, pawn); },
            .Prediction = net.PredictionPolicy,
            .Replay =
                [this](Scene& world, const u64 tick, const PlayerInput& input)
            {
                // Feed the recorded input to the local seat, then advance the Sim phase for this tick
                // with IsReplay set: InputMappingSystem leaves the fed input alone and side-effecting
                // systems gate their effects, while control + movement re-derive the predicted state.
                bool fed = false;
                world.Each<SeatInput, PlayerInput>(
                    [&](const Entity, const SeatInput&, PlayerInput& seatInput)
                    {
                        if (!fed)
                        {
                            seatInput = input;
                            fed = true;
                        }
                    });
                const f32 simDelta =
                    1.0f / static_cast<f32>(m_Info.World ? m_Info.World->SimTickRate : 60u);
                world.TickSimulationPhase(SceneSystem::Phase::Sim, simDelta,
                                          BuildSystemContext(world, RoleForWorld(m_ManagedWorld),
                                                             PointerRouting{}, tick, 0.0f, false,
                                                             /*isReplay=*/true));
            },
            // Per-key reconcile tolerances, unset by default (every join uses the shared value): a
            // world in a non-metre unit supplies its own so its client does not reconcile unbounded
            // drift as "matched".
            .WorldTolerances = net.ClientWorldTolerances,
            // The controller converts RTT/jitter to a tick lead at the sim rate; its margin carries
            // the snapshot-cadence staleness plus the two-tick buffered-input cushion beyond the
            // round-trip estimate. The world drive reads its target to seed and slew the sim clock.
            .TickSync =
                Net::TickSyncSettings{
                    .TickRate = m_Info.World ? m_Info.World->SimTickRate : 60u,
                    .MarginTicks = static_cast<f32>(net.SnapshotIntervalTicks) + 2.0f,
                },
            // Match each joined world's decoder dequantization grid to the server's wire quantization,
            // with a per-key override for a world of a different spatial envelope.
            .Quantization = Net::QuantizationSettings{.PositionQuantum = net.PositionQuantum,
                                                      .PositionExtent = net.PositionExtent,
                                                      .RotationBits = net.RotationBits},
            .WorldQuantization = net.ClientWorldQuantization,
            // A make-before-break directed travel leaves its departed join once the destination is
            // ready: close that join's runner world, dropping its net state — the "leave" of this plan.
            .OnLeaveWorld = [this](const Net::JoinId join) { CloseJoinedWorld(join); },
        });

        Log::Info("Joining {}:{}", host, port);
        return {};
    }

    VoidResult Application::StartHosting()
    {
        VE_ASSERT(m_Info.World, "StartHosting requires a managed world (ApplicationInfo::World)");
        if (m_Net)
        {
            return std::unexpected(string("a net mode is already active"));
        }
        VE_ASSERT(m_WorldRunner->ResolveWorld(m_ManagedWorld) != nullptr,
                  "StartHosting before the managed world is opened");
        // The managed world's level is echoed to a joining client (the reply names the level it loads).
        return StartServer(m_WorldLevel.Id());
    }

    VoidResult Application::Connect(const string& host, const u16 port)
    {
        VE_ASSERT(m_Info.World, "Connect requires a managed world (ApplicationInfo::World)");
        if (m_Net)
        {
            return std::unexpected(string("a net mode is already active"));
        }
        const u16 resolved = port != 0 ? port : m_Info.Net.value_or(GameNetInfo{}).Port;
        return ConnectClient(host, resolved);
    }

    WorldInstanceId Application::JoinWorld(const Net::WorldKey& key, const optional<bool> standing)
    {
        VE_ASSERT(m_Net && m_Net->ClientHost,
                  "JoinWorld requires an active client connection (Connect or --join)");

        // Open a fresh runner world the join's reply installs its scene into (LoadClientLevel), started
        // once the reply loads it. It ticks Client-tier and carries its own input send window, so a
        // joined gameplay world never collides with the managed world or another join.
        const WorldInstanceId world = m_WorldRunner->OpenWorld(WorldOpenInfo{
            .SimTickRate = m_Info.World ? m_Info.World->SimTickRate : 60u,
            .StartSimulation = false,
        });
        m_Net->WorldRoles[world.Value] = NetRole::Client;
        m_Net->ClientWorlds[world.Value].Send = InputSendBuffer(
            InputSendBuffer::Settings{.Redundancy = m_Net->Info.InputRedundancyTicks});
        m_Net->PendingJoinWorlds.push_back(world);
        // JoinWorld never presents by itself (the consumer rebinds a viewport), so the unset
        // default records a standing join — a data world held across reconnects.
        m_Net->ClientHost->Join(key, {}, /*present=*/false, standing);
        return world;
    }

    WorldInstanceId Application::JoinWorld(const Net::WorldKey& key, const WorldInstanceId adopt,
                                           const optional<bool> standing)
    {
        VE_ASSERT(m_Net && m_Net->ClientHost,
                  "JoinWorld requires an active client connection (Connect or --join)");
        World* resolved = m_WorldRunner->ResolveWorld(adopt);
        VE_ASSERT(resolved != nullptr, "JoinWorld adopt target is not an open world");
        VE_ASSERT(m_Net->ClientWorlds.contains(adopt.Value) &&
                      m_Net->ClientWorlds[adopt.Value].Started,
                  "JoinWorld adopt target's scene is not a started client world");

        // Adopt in place: the join binds to the existing world's live scene rather than opening a fresh
        // one and loading a level. No new WorldRole / ClientWorlds entry — the two joins share the one
        // world's per-world client state; their wire-id spaces are disjoint (kept per ReplicationClient).
        // Presentation for the session record follows the adopted world's viewport binding: a swap
        // under a presented scene is the account's new gameplay world.
        bool present = false;
        const usize viewports = m_ManagedViewports->GetCount();
        for (usize i = 0; i < viewports; ++i)
        {
            if (m_ManagedViewports->GetViewportWorld(i) == adopt)
            {
                present = true;
                break;
            }
        }
        m_Net->ClientHost->JoinInto(key, resolved->GetScene(), {}, present, standing);
        return adopt;
    }

    void Application::LeaveWorld(const Net::JoinId join)
    {
        VE_ASSERT(m_Net && m_Net->ClientHost,
                  "LeaveWorld requires an active client connection (Connect or --join)");
        // The ClientHost removes the join's footprint and notifies the server; the OnLeaveWorld hook
        // (CloseJoinedWorld) closes the runner world only when no peer join still presents its scene.
        m_Net->ClientHost->Leave(join);
    }

    void Application::StopNet()
    {
        // A durability point: every dirty session record saves before the host (and its
        // connections) go away.
        if (m_Sessions)
        {
            m_Sessions->SaveAll();
        }
        // Dropping the host closes its connections and returns the managed world to Server-tier with no
        // transport (the standalone authority model); the world scenes themselves are untouched.
        m_Net.reset();
    }

    void Application::BuildWorldDirectory()
    {
        const GameNetInfo net = m_Info.Net.value_or(GameNetInfo{});
        // The session registry lives beside the directory in every role: standalone travels record
        // into it, and a mounted ServerHost borrows it (ServerHostInfo::Sessions).
        m_Sessions = Net::SessionRegistry::Create(Net::SessionRegistryInfo{
            .Types = &m_TypeRegistry,
            .TransformOnReattach = net.TransformOnReattach,
            .CaptureTravelPose = net.CaptureTravelPose,
            .LoadSession = net.LoadSession,
            .SaveSession = net.SaveSession,
        });
        m_Directory = WorldDirectory::Create(WorldDirectoryInfo{
            .MaxHostedWorlds = net.MaxHostedWorlds,
            .MaxJoinedWorldsPerConnection = net.MaxJoinedWorldsPerConnection,
            .MaxPlayersPerInstance = net.MaxPlayersPerInstance,
            .IdleKeepWarmDwell = net.IdleKeepWarmDwell,
            .Runner = m_WorldRunner.get(),
            .Authorize = net.Authorize,
            .WorldFactory = net.WorldFactory,
            .Placement = net.Placement,
            .CloseWorld = net.CloseWorld,
        });
        // The managed world is a never-reaped bucket: "you start here" is just its being pre-registered,
        // not app bookkeeping — the home-star special case dies.
        if (m_ManagedWorld.IsValid())
        {
            m_Directory->Register(Net::DefaultWorldKey, m_ManagedWorld);
        }
    }

    bool Application::IsWorldPresented(const WorldInstanceId world) const
    {
        if (!world.IsValid())
        {
            return false;
        }

        // The engine-owned bindings first: an indexed managed viewport, an overlay's bound viewport, or
        // an in-flight rebind destined for the world (which counts for its whole wait).
        if (m_ManagedViewports != nullptr && m_ManagedViewports->IsWorldPresented(world))
        {
            return true;
        }

        // Then any viewport a consumer registered and drives itself. Those name a Scene in the
        // ViewState they push rather than a world handle, so the world's scene identity is what
        // resolves them — an Offscreen viewport counts too, since a material or panel samples its
        // output. Without this arm a self-driven viewport's world would read as unpresented and lose
        // the per-world work presentation gates.
        const World* resolved = m_WorldRunner->ResolveWorld(world);
        if (resolved == nullptr || resolved->LiveScene == nullptr)
        {
            return false;
        }
        const Scene* scene = resolved->LiveScene;
        return std::ranges::any_of(m_Compositor.GetViewports(),
                                   [scene](const Renderer::Viewport* viewport)
                                   { return viewport->GetPresentedScene() == scene; });
    }

    void Application::SyncPresentationPins()
    {
        if (!m_Directory)
        {
            return;
        }

        // The worlds presentation shows this frame: each managed viewport's applied binding plus any
        // pending rebind destination (a pending destination counts, so a presented world is never reaped
        // in its own rebind gap).
        std::unordered_set<u64> present;
        const usize count = m_ManagedViewports->GetCount();
        for (usize i = 0; i < count; ++i)
        {
            const WorldInstanceId world = m_ManagedViewports->GetViewportWorld(i);
            if (world.IsValid() && m_Directory->Contains(world))
            {
                present.insert(world.Value);
            }
            if (const optional<WorldInstanceId> pending =
                    m_ManagedViewports->GetPendingViewportWorld(i);
                pending && pending->IsValid() && m_Directory->Contains(*pending))
            {
                present.insert(pending->Value);
            }
        }

        const f64 now = static_cast<f64>(Time::Now());
        // Pin worlds newly present; unpin worlds that left presentation, so the dwell owns their fate.
        // The pin carries the local account — a presented world is the local player's join, so
        // MembersOf reports it beside connected accounts (invalid on a dedicated host, presence only).
        for (const u64 value : present)
        {
            if (!m_PinnedWorlds.contains(value))
            {
                m_Directory->Pin(WorldInstanceId{.Value = value}, m_LocalAccount);
            }
        }
        for (auto it = m_PinnedWorlds.begin(); it != m_PinnedWorlds.end();)
        {
            if (!present.contains(*it))
            {
                m_Directory->Unpin(WorldInstanceId{.Value = *it}, now, m_LocalAccount);
                it = m_PinnedWorlds.erase(it);
            }
            else
            {
                ++it;
            }
        }
        m_PinnedWorlds = std::move(present);
    }

    void Application::ReapDirectory()
    {
        // While hosting, the ServerHost's Pump reaps the shared directory (and drops its per-world
        // replication state); standalone, Application owns the reap. Skip it when a host is active so a
        // world is never reaped twice.
        if (m_Directory && GetServerHost() == nullptr)
        {
            (void)m_Directory->ReapIdle(static_cast<f64>(Time::Now()));
        }
    }

    VoidResult Application::TravelStandalone(const TravelInfo& info)
    {
        // A standalone resolve is the local player's own request: no connection, the local account.
        const WorldResolveResult resolve =
            m_Directory->Resolve(Net::JoinRequestInfo{.Connection = Net::ConnectionId{},
                                                      .Account = m_LocalAccount,
                                                      .Key = info.Key,
                                                      .Payload = info.Payload,
                                                      .Profile = GetLocalProfile()},
                                 /*heldWorlds=*/0);
        if (resolve.Outcome == WorldResolveOutcome::Denied)
        {
            return std::unexpected(
                fmt::format("travel denied: {}", static_cast<u32>(resolve.Reason)));
        }

        const Net::SessionDurability durability =
            Net::ResolveSessionDurability(info.Present, info.Standing);

        // The session record follows as a side effect of the travel, exactly as a hosted join's
        // does: a presenting travel is the local account's gameplay entry (params double as the
        // arrival pose until a capture refreshes it), a standing one enters the standing list, an
        // opted-out one enters nothing.
        if (m_Sessions && m_LocalAccount.IsValid())
        {
            switch (durability)
            {
            case Net::SessionDurability::Gameplay:
                m_Sessions->RecordGameplay(m_LocalAccount, info.Key, info.Payload, info.Payload);
                break;
            case Net::SessionDurability::Standing:
                m_Sessions->RecordStandingJoin(m_LocalAccount, info.Key);
                break;
            case Net::SessionDurability::None:
                break;
            }
        }

        // A non-presenting standing travel warms its world under a local presence: with no connection
        // to report a join, the local standing membership is a directory pin, so keep-warm accounting
        // counts the local member and the bucket survives until every presence (this hold, remote
        // joins) is gone. LeaveStanding withdraws it. A presenting travel takes no standing pin — its
        // presentation pin (SyncPresentationPins) holds it instead.
        if (durability == Net::SessionDurability::Standing)
        {
            AcquireLocalStanding(info.Key, resolve.World);
        }

        // Present-on-ready rebind onto the destination when presenting: the pin follows at the rebind
        // apply point (SyncPresentationPins reads the pending destination, so it is pinned immediately),
        // and the departed world is unpinned there too — the dwell then owns its fate. A non-presenting
        // resolve opens/places the world but takes no presentation pin, so an unpinned world's fate is
        // the dwell's.
        if (info.Present && info.ViewportIndex < m_ManagedViewports->GetCount())
        {
            m_ManagedViewports->RebindWorldWhenReady(info.ViewportIndex, resolve.World);
        }
        return {};
    }

    void Application::AcquireLocalStanding(const Net::WorldKey& key, const WorldInstanceId world)
    {
        if (!m_Directory)
        {
            return;
        }
        // Idempotent per key: a repeated standing hold on a key already pinned takes no second presence,
        // matching SessionRegistry::RecordStandingJoin's dedupe.
        if (const auto [it, inserted] = m_LocalStandingWorlds.try_emplace(key, world); inserted)
        {
            m_Directory->Pin(world, m_LocalAccount);
        }
    }

    void Application::LeaveStanding(const Net::WorldKey& key)
    {
        const auto it = m_LocalStandingWorlds.find(key);
        if (it == m_LocalStandingWorlds.end())
        {
            return;
        }
        const WorldInstanceId world = it->second;
        m_LocalStandingWorlds.erase(it);
        if (m_Directory)
        {
            m_Directory->Unpin(world, static_cast<f64>(Time::Now()), m_LocalAccount);
        }
        if (m_Sessions && m_LocalAccount.IsValid())
        {
            m_Sessions->RemoveStandingJoin(m_LocalAccount, key);
        }
    }

    VoidResult Application::HoldWorldWarm(const Net::WorldKey& key)
    {
        VE_ASSERT(m_Directory, "HoldWorldWarm requires a managed world (ApplicationInfo::World)");

        // Idempotent per key: a repeated hold on a key already warm-pinned takes no second pin.
        if (m_WarmPinnedWorlds.contains(key))
        {
            return {};
        }

        // A requester-less resolve: no connection and the invalid account, since this hold belongs to
        // the process infrastructure, not a player. A factory keying off the requester reads the
        // invalid account as "no specific requester" (see WorldDirectoryInfo::WorldFactory).
        const Net::Blob empty;
        const WorldResolveResult resolve =
            m_Directory->Resolve(Net::JoinRequestInfo{.Connection = Net::ConnectionId{},
                                                      .Account = Net::AccountId{},
                                                      .Key = key,
                                                      .Payload = empty},
                                 /*heldWorlds=*/0);
        if (resolve.Outcome == WorldResolveOutcome::Denied)
        {
            return std::unexpected(
                fmt::format("warm hold denied: {}", static_cast<u32>(resolve.Reason)));
        }

        // An accountless pin — presence with no recorded member — so keep-warm accounting holds the
        // bucket resident beside any joins, and MembersOf never reports the hold.
        m_WarmPinnedWorlds.emplace(key, resolve.World);
        m_Directory->Pin(resolve.World);
        return {};
    }

    void Application::ReleaseWorldWarm(const Net::WorldKey& key)
    {
        const auto it = m_WarmPinnedWorlds.find(key);
        if (it == m_WarmPinnedWorlds.end())
        {
            return;
        }
        const WorldInstanceId world = it->second;
        m_WarmPinnedWorlds.erase(it);
        if (m_Directory)
        {
            m_Directory->Unpin(world, static_cast<f64>(Time::Now()));
        }
    }

    VoidResult Application::Travel(const TravelInfo& info)
    {
        VE_ASSERT(m_Directory, "Travel requires a managed world (ApplicationInfo::World)");

        // Client: the server resolves a payload-parameterized key and directs the join (make-before-
        // break). The client never self-resolves such a key. A presenting travel records its
        // viewport target so the directed join's install rebinds the right managed viewport.
        if (m_Net && m_Net->ClientHost != nullptr && GetNetRole() == NetRole::Client)
        {
            if (info.Present)
            {
                m_Net->PresentTargets[info.Key] = info.ViewportIndex;
            }
            m_Net->ClientHost->Travel(info.Key, info.Payload, info.Present, info.Standing);
            return {};
        }

        // Standalone and listen host: resolve locally through the directory, then present. The local
        // resolution is join-visible, so a remote joiner of the same key converges on it.
        return TravelStandalone(info);
    }

    void Application::DrainRequestComponents()
    {
        RequestDispatch dispatch;

        dispatch.StopNet = [this](const WorldInstanceId, const StopNetRequest&, string&)
        {
            // Client disconnect or server stop; a harmless no-op when standalone, so always handled.
            StopNet();
            return RequestResult::Handled;
        };

        dispatch.Host = [this](const WorldInstanceId world, const HostRequest&, string& error)
        {
            if (RoleForWorld(world) == NetRole::Client)
            {
                error = "cannot start hosting from a client-tier world";
                return RequestResult::Failed;
            }
            if (VoidResult result = StartHosting(); !result)
            {
                error = std::move(result.error());
                return RequestResult::Failed;
            }
            return RequestResult::Handled;
        };

        dispatch.Connect =
            [this](const WorldInstanceId world, const ConnectRequest& request, string& error)
        {
            if (VoidResult result = Connect(request.Host, request.Port); !result)
            {
                error = std::move(result.error());
                return RequestResult::Failed;
            }
            // The connect-and-enter front door: a non-default Join key travels after connecting, an
            // invalid (default) key is connect-only.
            if (!(request.Join == Net::WorldKey{}))
            {
                if (VoidResult result = Travel(TravelInfo{.Key = request.Join,
                                                          .Payload = request.Payload,
                                                          .ViewportIndex = 0,
                                                          .Present = true});
                    !result)
                {
                    error = std::move(result.error());
                    return RequestResult::Failed;
                }
            }
            return RequestResult::Handled;
        };

        dispatch.Travel = [this](const WorldInstanceId, const TravelRequest& request, string& error)
        {
            if (VoidResult result = Travel(TravelInfo{.Key = request.Destination,
                                                      .Payload = request.Payload,
                                                      .ViewportIndex = request.ViewportIndex,
                                                      .Present = request.Present,
                                                      .Standing = request.Standing});
                !result)
            {
                error = std::move(result.error());
                return RequestResult::Failed;
            }
            return RequestResult::Handled;
        };

        dispatch.Exit = [this](const WorldInstanceId, const ExitRequest&, string&)
        {
            RequestExit();
            return RequestResult::Handled;
        };

        dispatch.Focus = [this](const WorldInstanceId, const FocusRequest& request, string& error)
        {
            // The engine owns the per-seat token across frames so a stateless system can drive
            // input focus; the reconcile composes with overlay / SeatFocusScope tokens.
            return ReconcileFocusRequest(*m_InputRouter, m_FocusRequestTokens, request, error);
        };

        DrainRequests(*m_WorldRunner, dispatch);
    }

    WorldInstanceId Application::NextJoinTargetWorld()
    {
        // The reply installs into the world queued for this join (FIFO in reply order). A join with no
        // queued target — a server-directed travel the ClientHost issued itself — opens a fresh runner
        // world here, so it never clobbers the managed world or another join's scene.
        if (!m_Net->PendingJoinWorlds.empty())
        {
            const WorldInstanceId target = m_Net->PendingJoinWorlds.front();
            m_Net->PendingJoinWorlds.pop_front();
            return target;
        }
        const WorldInstanceId target = m_WorldRunner->OpenWorld(WorldOpenInfo{
            .SimTickRate = m_Info.World ? m_Info.World->SimTickRate : 60u,
            .StartSimulation = false,
        });
        m_Net->WorldRoles[target.Value] = NetRole::Client;
        m_Net->ClientWorlds[target.Value].Send = InputSendBuffer(
            InputSendBuffer::Settings{.Redundancy = m_Net->Info.InputRedundancyTicks});
        return target;
    }

    Scene* Application::OpenEmptyClientWorld()
    {
        const WorldInstanceId target = NextJoinTargetWorld();

        // A level-less data world: nothing to load — its content arrives entirely from the stream.
        // The installed scene carries a simulation running no systems so the world starts and ticks
        // through the ordinary joined-world path; presentation bridges read it, no system advances it.
        Unique<Scene> scene = Scene::Create(m_TypeRegistry);
        scene->SetSimulation(CreateUnique<SceneSimulation>(m_SystemRegistry, vector<SystemId>{}));
        return &m_WorldRunner->InstallScene(target, std::move(scene));
    }

    Scene* Application::LoadClientLevel(const AssetId id)
    {
        const WorldInstanceId target = NextJoinTargetWorld();

        // The accept names the level; load it with the server-authoritative authored entities skipped
        // (they arrive from the spawn stream) and keep the level handle resident in the target world's
        // state. The residency batch is held for OnWorldLoaded when the scene starts. The loaded scene is
        // installed as the target world's scene, so the runner owns it — the ClientHost applies the
        // stream into a runner-owned world.
        const AssetResult<AssetHandle<Level>> level = m_AssetManager->LoadSync<Level>(id);
        VE_ASSERT(level.has_value(), "client level load failed: {}", level.error().Detail);

        NetState::ClientWorldState& state = m_Net->ClientWorlds[target.Value];
        state.Level = *level;
        LevelInstance instance = state.Level.Get()->LoadInto(
            *m_AssetManager, m_SystemRegistry, LevelLoadInfo{.SkipServerAuthoritative = true});
        state.Pending = std::move(instance.Pending);
        // The managed world's level handle is also mirrored for GetWorldLevel and StartHosting's reply.
        if (target == m_ManagedWorld)
        {
            m_WorldLevel = state.Level;
        }
        return &m_WorldRunner->InstallScene(target, std::move(instance.World));
    }

    void Application::StartWorldScene(const WorldInstanceId world, Scene& scene)
    {
        // Seed the managed viewport only from the managed world; a joined world becomes presented
        // through the consumer's RebindManagedViewport, whose apply re-seeds the viewport from the
        // destination's authored LevelRenderSettings.
        if (world == m_ManagedWorld)
        {
            SeedViewportFromWorld(scene);
        }

        NetState::ClientWorldState& state = m_Net->ClientWorlds[world.Value];
        OnWorldLoaded(world, scene, state.Pending);

        // The residency batch existed only to hold the join-loaded assets resident across the
        // deferred gap between the client level load and this start; the spawned scene's components
        // now hold those handles, so release the batch's redundant cache-entry refs. Keeping it would
        // pin every join-loaded GPU resource for the whole session and leave the last ref in this
        // state — dropped only after ~Context destroys the VMA allocator, tripping its leak
        // assert. The standalone path's batch is a local that dies here for the same reason.
        state.Pending = ResidencyBatch{};

        scene.StartSimulation(MakeWorldContext(world));
    }

    SystemContext Application::MakeWorldContext(const WorldInstanceId world) const
    {
        return SystemContext{.Assets = *m_AssetManager,
                             .Input = *m_Input,
                             .Tasks = *m_TaskSystem,
                             .Audio = m_AudioDevice->GetEngine(),
                             .Role = RoleForWorld(world)};
    }

    void Application::PresentJoinedWorld(const Net::JoinId join, const WorldInstanceId world)
    {
        if (m_Net->ClientHost == nullptr || !m_Net->ClientHost->IsPresenting(join))
        {
            return;
        }
        // The travel that requested this key recorded its viewport target; a presenting join with
        // no record (an unprompted directed travel, a reattach's gameplay restore) fronts on 0.
        usize index = 0;
        if (const auto it = m_Net->PresentTargets.find(m_Net->ClientHost->JoinKey(join));
            it != m_Net->PresentTargets.end())
        {
            index = it->second;
            m_Net->PresentTargets.erase(it);
        }
        if (index >= m_ManagedViewports->GetCount() ||
            m_ManagedViewports->GetViewportWorld(index) == world)
        {
            return;
        }
        Log::Info("Presenting joined world {} on managed viewport {}", world.Value, index);
        m_ManagedViewports->RebindWorldWhenReady(index, world);
    }

    void Application::CloseJoinedWorld(const Net::JoinId join)
    {
        if (!m_Net)
        {
            return;
        }
        const auto it = m_Net->JoinWorlds.find(join);
        if (it == m_Net->JoinWorlds.end())
        {
            return;
        }
        const WorldInstanceId world = it->second;
        m_Net->JoinWorlds.erase(it);

        // Keep the world when a peer join still presents its scene (an adopt-in-place swap: the departed
        // join's scene is the destination join's). Only when this was the sole join on the world — a
        // fresh-world join (today's kind) — is the runner world closed, reproducing current teardown.
        const World* resolved = m_WorldRunner->ResolveWorld(world);
        const Scene* scene = resolved != nullptr ? &resolved->GetScene() : nullptr;
        if (scene != nullptr && m_Net->ClientHost != nullptr)
        {
            for (const Net::JoinId other : m_Net->ClientHost->Joins())
            {
                if (m_Net->ClientHost->World(other) == scene)
                {
                    return; // a peer join still streams into this scene — leave the world standing
                }
            }
        }

        m_Net->WorldRoles.erase(world.Value);
        m_Net->ClientWorlds.erase(world.Value);
        m_WorldRunner->CloseWorld(world);
    }

    Net::JoinId Application::ClientJoinForWorld(const WorldInstanceId world) const
    {
        if (!m_Net || m_Net->ClientHost == nullptr)
        {
            return Net::ControlJoinId;
        }
        const World* resolved = m_WorldRunner->ResolveWorld(world);
        if (resolved == nullptr)
        {
            return Net::ControlJoinId;
        }
        const Scene* scene = &resolved->GetScene();
        for (const Net::JoinId join : m_Net->ClientHost->Joins())
        {
            if (m_Net->ClientHost->World(join) == scene)
            {
                return join;
            }
        }
        return Net::ControlJoinId;
    }

    void Application::NotifyPossession(Scene& world, const Entity pawn)
    {
        const std::pair<const Scene*, Entity> notice{&world, pawn};
        if (std::ranges::find(m_PossessionNotices, notice) != m_PossessionNotices.end())
        {
            return;
        }
        m_PossessionNotices.push_back(notice);
        OnClientPossession(world, pawn);
    }

    void Application::SyncLocalControl()
    {
        for (const Unique<World>& world : m_WorldRunner->GetWorlds())
        {
            if (world == nullptr || world->LiveScene == nullptr)
            {
                continue;
            }
            // A world no managed viewport presents collects no seat, which clears its markers: the
            // pass is what makes a viewport rebind, a seat teardown, and a world leaving the screen
            // all leave nothing stale behind.
            m_PresentingSeats.clear();
            if (m_ManagedViewports)
            {
                m_ManagedViewports->CollectPresentingSeats(world->Id, m_PresentingSeats);
            }
            m_LocalControlChanges.clear();
            ReconcileLocalControl(world->GetScene(), m_PresentingSeats, &m_LocalControlChanges);
            for (const LocalControlChange& change : m_LocalControlChanges)
            {
                NotifyPossession(world->GetScene(), change.Pawn);
            }
        }
        m_PossessionNotices.clear();
    }

    void Application::PumpNet()
    {
        if (!m_Net)
        {
            return;
        }

        VE_PROFILE_SCOPE("Net/Pump");

        // Keep the JoinId → runner world map current before the join/apply flow, so a directed travel's
        // leave (fired from inside the pump) resolves the departed join's world from this frame's map.
        if (m_Net->ClientHost != nullptr)
        {
            m_Net->JoinWorlds.clear();
            for (const auto& [worldValue, role] : m_Net->WorldRoles)
            {
                if (role != NetRole::Client)
                {
                    continue;
                }
                const WorldInstanceId world{.Value = worldValue};
                const Net::JoinId join = ClientJoinForWorld(world);
                if (join != Net::ControlJoinId)
                {
                    m_Net->JoinWorlds[join] = world;
                }
            }
        }

        // One ClientHost multiplexes every joined world over one connection, so advance the shared
        // join/apply flow once before the per-world drive rather than once per net-active client world.
        if (m_Net->ClientHost != nullptr)
        {
            VE_PROFILE_SCOPE("Net/ClientPump");
            m_Net->ClientHost->Pump(static_cast<f64>(Time::Now()));
        }

        // Pump each net-active world once: a Server-tier world flushes its host stream, a Client-tier
        // world starts its loaded scene and sends its stamped input. A standalone Server-tier world
        // (absent from the map) is never pumped.
        for (const auto& [worldValue, role] : m_Net->WorldRoles)
        {
            PumpNetWorld(WorldInstanceId{.Value = worldValue}, role);
        }

#if defined(VE_PROFILE) && VE_PROFILE
        // The connection RTT and the server's per-world replication bytes are maintained each frame
        // regardless; sampling the existing figures here only puts them on the trace timeline.
        if (m_Net->Client != nullptr && m_Net->Client->State() == Net::ClientState::Connected)
        {
            VE_PROFILE_COUNTER("Net/Client/RttMs",
                               static_cast<f64>(m_Net->Client->Server().RttEstimate()) * 1000.0);
        }
        if (m_Net->Server != nullptr)
        {
            u64 replicationBytes = 0;
            for (const auto& [worldValue, role] : m_Net->WorldRoles)
            {
                if (role == NetRole::Server)
                {
                    replicationBytes += m_Net->Server->ReplicationBytesForWorld(
                        WorldInstanceId{.Value = worldValue});
                }
            }
            VE_PROFILE_COUNTER("Net/Server/ReplicationBytes", static_cast<f64>(replicationBytes));
        }
#endif
    }

    void Application::PumpNetWorld(const WorldInstanceId world, const NetRole role)
    {
        const f64 now = static_cast<f64>(Time::Now());

        if (role == NetRole::Server)
        {
            if (m_Net->Server == nullptr)
            {
                return;
            }
            VE_PROFILE_SCOPE("Net/ServerPump");
            // The world is generated + streamed keyed to the last completed tick (its just-ticked
            // state); the per-step SetChangeTick already stamped this frame's mutations.
            m_Net->Server->Pump(now, WorldSimTick(world));

            // Ingest each connection's redundant input into its jitter buffer for next frame's ticks
            // to consume, pruning a departed connection's buffer.
            IngestConnectionInputs(*m_Net->Server, m_Net->Jitter, InputJitterBuffer::Settings{},
                                   m_TypeRegistry);
            return;
        }

        if (m_Net->ClientHost == nullptr)
        {
            return;
        }

        // Client: the shared ClientHost was already pumped in PumpNet. Resolve this world's join (invalid
        // until its reply loads its scene), start its scene once, then send its stamped input window.
        const Net::JoinId join = ClientJoinForWorld(world);
        if (join == Net::ControlJoinId)
        {
            return;
        }

        NetState::ClientWorldState& state = m_Net->ClientWorlds[world.Value];
        if (!state.Started)
        {
            World* resolved = m_WorldRunner->ResolveWorld(world);
            if (resolved != nullptr)
            {
                StartWorldScene(world, resolved->GetScene());
                state.Started = true;
                PresentJoinedWorld(join, world);
            }
        }

        // Acknowledge the highest applied server tick for this world so the server advances this
        // connection's delta baselines (and gates its snapshots against them) rather than re-sending
        // full state forever. Tag the input with the world's JoinId so the server demuxes it to the
        // right instance.
        if (m_Net->Client->State() == Net::ClientState::Connected)
        {
            const vector<u8> packet =
                state.Send.Encode(m_Net->ClientHost->LastServerTick(join), m_TypeRegistry);
            (void)m_Net->Client->Server().Send(Net::Channel::UnreliableSequenced,
                                               Net::EncodeWorldEnvelope(join, packet));
        }
    }

    void Application::FeedServerSeatInputs(const WorldInstanceId world, const u64 tick)
    {
        World* resolved = m_WorldRunner->ResolveWorld(world);
        if (m_Net && m_Net->Server && resolved != nullptr)
        {
            // Scheduled consume: the client runs its tick ahead of the server (the tick-offset slew),
            // so the input it stamped at this tick has arrived by the time the server reaches it.
            FeedSeatInputs(*m_Net->Server, m_Net->Jitter, world, resolved->GetScene(), tick);
        }
    }

    void Application::StampClientInput(const WorldInstanceId world, const u64 clientTick)
    {
        World* resolved = m_WorldRunner->ResolveWorld(world);
        if (m_Net && resolved != nullptr)
        {
            // Stamp into this world's own send window; a multiplexed client keeps one per joined world.
            StampLocalSeatInput(m_Net->ClientWorlds[world.Value].Send, resolved->GetScene(),
                                clientTick);
        }
    }

    namespace
    {
        // The scene-local keyboard/mouse seat a captured pointer routes to: a (Viewer, SeatInput)
        // with UsesKeyboardMouse, preferring the locally-owned one when a peer holds several and
        // falling back to the first, Entity::Null when the scene has none. With a single such seat
        // the first is that one, so nothing regresses.
        Entity FirstKeyboardSeat(const Scene& scene)
        {
            Entity firstKeyboard = Entity::Null;
            Entity localKeyboard = Entity::Null;
            scene.Each<Viewer, SeatInput>(
                [&](const Entity seat, const Viewer&, const SeatInput& devices)
                {
                    if (!devices.UsesKeyboardMouse)
                    {
                        return;
                    }
                    if (firstKeyboard == Entity::Null)
                    {
                        firstKeyboard = seat;
                    }
                    if (localKeyboard == Entity::Null && IsLocallyOwned(scene, seat))
                    {
                        localKeyboard = seat;
                    }
                });
            return localKeyboard != Entity::Null ? localKeyboard : firstKeyboard;
        }
    }

    Application::ScopedPointer Application::ComputePointerRouting() const
    {
        if (!m_WorldRunner->HasWorlds())
        {
            return {};
        }
        const World* managed = m_WorldRunner->ResolveWorld(m_ManagedWorld);
        const Scene* managedScene = managed != nullptr ? &managed->GetScene() : nullptr;

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
            // viewport's scene, or the managed world when the cursor seat has no viewport (the default
            // single-seat path). Resolve that scene's keyboard seat scene-locally.
            const Scene* scene = owner != nullptr ? owner->GetPresentedScene() : managedScene;
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

    Audio::AudioEngine& Application::GetAudioEngine()
    {
        VE_ASSERT(m_AudioDevice, "GetAudioEngine before Run(): the audio device exists only once "
                                 "Run() has initialized the engine");
        return m_AudioDevice->GetEngine();
    }

    SystemContext Application::BuildSystemContext(const Scene& scene, const NetRole role,
                                                  const PointerRouting& pointer, const u64 tick,
                                                  const f32 alpha, const bool firstStepThisFrame,
                                                  const bool isReplay) const
    {
        SystemContext context{
            .Assets = *m_AssetManager,
            .Input = *m_Input,
            .Tasks = *m_TaskSystem,
            .Audio = m_AudioDevice->GetEngine(),
            .Pointer = pointer,
            .Tick = tick,
            .Alpha = alpha,
            .Role = role,
            // The focus gate every seat's input resolution reads: true only while the router reports
            // the cursor seat gameplay-focused, false headless (no window owns focus).
            .GameplayFocused = m_InputRouter->IsGameplayFocused(),
            .FirstStepThisFrame = firstStepThisFrame,
            .IsReplay = isReplay};

        // Resolve the sim's primary presenting viewport — the first registered Presented viewport
        // whose retained scene is this one — for the view descriptor and debug-draw sink. The
        // retained view is last frame's push (view pushes run after ticks); a never-pushed viewport
        // presents no scene, so it never matches and View stays nullopt.
        for (const Renderer::Viewport* viewport : m_Compositor.GetViewports())
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
        m_ManagedViewports->Reconfigure(viewports);
    }

    void Application::RebindManagedViewport(const usize index, const WorldInstanceId world)
    {
        m_ManagedViewports->RebindWorld(index, world);
    }

    void Application::RebindManagedViewportWhenReady(const usize index, const WorldInstanceId world)
    {
        m_ManagedViewports->RebindWorldWhenReady(index, world);
    }

    WorldInstanceId Application::GetManagedViewportWorld(const usize index) const
    {
        return m_ManagedViewports->GetViewportWorld(index);
    }

    optional<WorldInstanceId> Application::GetPendingManagedViewportWorld(const usize index) const
    {
        return m_ManagedViewports->GetPendingViewportWorld(index);
    }

    WorldInstanceId Application::GetAbandonedManagedPresentWorld(const usize index) const
    {
        return m_ManagedViewports->GetAbandonedPresentWorld(index);
    }

    void Application::RegisterViewport(Renderer::Viewport& viewport)
    {
        m_Compositor.RegisterViewport(viewport);
    }

    void Application::RegisterCapture(Renderer::SceneCapture& capture)
    {
        m_Compositor.RegisterCapture(capture);
    }

    void Application::SetWorldPaused(const WorldInstanceId world, const bool paused)
    {
        m_WorldRunner->SetWorldPaused(world, paused);
    }

    bool Application::IsWorldPaused(const WorldInstanceId world) const
    {
        return m_WorldRunner->IsWorldPaused(world);
    }

    u64 Application::GetSimTick() const
    {
        return WorldSimTick(m_ManagedWorld);
    }

    const AssetHandle<Level>& Application::GetWorldLevel(WorldInstanceId) const
    {
        return m_WorldLevel;
    }

    Renderer::ViewState& Application::GetWorldViewState(WorldInstanceId)
    {
        return m_WorldView;
    }

    i32 Application::Run(vector<string> arguments)
    {
        // Parse argv (without the program name) once; the engine consumes the recognised options
        // itself and a game can read them back through GetLaunchArguments(). The app object is
        // constructed before Run, so m_Info's declared options are available to widen the parse.
        const std::span<const string> tokens =
            arguments.empty() ? std::span<const string>{} : std::span(arguments).subspan(1);
        Result<LaunchArguments> parsed = LaunchArguments::Parse(tokens, m_Info.LaunchOptions);
        VE_ASSERT(parsed, "{}", parsed.error());
        m_LaunchArgs = std::move(*parsed);

        // `--headless` forces a windowed game exe into the dedicated-server posture before init: no
        // window, no swapchain, ImGui off. Applied here (ahead of Initialize) so the whole render path
        // is never built, not merely skipped per frame.
        if (m_LaunchArgs.Headless)
        {
            m_Info.Headless = true;
        }

        // `--no-render` clears the render tail for a headless run whose frames nothing reads. Applied
        // beside Headless so the whole frame policy is settled before Initialize.
        if (m_LaunchArgs.NoRender)
        {
            m_Info.HeadlessRendering = false;
        }

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

        // Run ends at its operations — work that must complete while the app is fully alive, in order.
        // The release that follows is the members' destruction, which reverse-declaration order
        // sequences (see the member declarations); Run does none of it.

        // Quiesce: drain the GPU and the worker pool so no in-flight continuation lands a resource on
        // an app member after teardown begins.
        m_RenderContext.WaitIdle();
        m_TaskSystem->WaitForAll();

        // The app's own shutdown operations, run while every engine service is still alive.
        OnShutdown();

        // The durability operation: flush every dirty session record through the SaveSession hook,
        // which dereferences derived-class state — so it cannot move to a destructor.
        if (m_Sessions)
        {
            m_Sessions->SaveAll();
        }

        // The status a RequestExit(i32) named, or 0 for a run that ended without naming one.
        return m_ExitStatus;
    }

    void Application::Frame()
    {
        // The frame boundary: advances the profiler's frame index once per frame and folds the
        // completed frame's aggregates. Not a scope — every later event this frame indexes against it.
        VE_PROFILE_FRAME();

        // Sample the per-frame profiler counters at the frame boundary (task pool depth, net
        // telemetry), the profiler's own once-per-frame point.
        SampleFrameCounters();

        const f32 delta = Time::Update();

        {
            VE_PROFILE_SCOPE("Frame/RequestDrain");

            // Apply the deferred managed-viewport work at the top of the frame, outside any
            // Scene/viewport-list iteration: it drops and constructs viewports (mutating the
            // drive-list), which must not run mid-drive. Regions resolve from each info's Layout here;
            // world rebinds run their departed-overlay detach and seat re-resolution through the
            // runner; present-on-ready rebinds accrue this frame's delta toward their ready timeout.
            m_ManagedViewports->ApplyPendingReconfigure(*m_WorldRunner, delta, m_WorldView);

            // Translate the managed viewports' world bindings into directory presence pins at the
            // rebind apply point (one-directional: presentation drives lifetime, never the reverse),
            // then reap the directory standalone (a host owns the reap when hosting). A presented
            // world — pending rebind destination included — is pinned and never reaped; a departed
            // world is unpinned to the dwell.
            SyncPresentationPins();
            ReapDirectory();

            // The standalone durability checkpoint; while hosting, the ServerHost's Pump owns the
            // shared registry's checkpoint (with its live pose refresh), so skip it there.
            if (m_Sessions && GetServerHost() == nullptr)
            {
                m_Sessions->Checkpoint(static_cast<f64>(Time::Now()));
            }

            // Drain the builtin request components at the same frame-safe point: a gameplay system
            // stamps one onto its world's scene to reach an application-level operation it cannot call
            // directly (host, connect, stop-net, travel, exit), and the engine carries them out here,
            // before the input snapshot and the world tick. Runs on the local Application only;
            // requests never ride the wire.
            DrainRequestComponents();

            // Deliver queued inbound game messages at the same frame-safe point: the hosts' pumps only
            // queue them, and dispatching here — outside any scene iteration, before the world tick —
            // means a channel handler observing scene state never runs mid-tick.
            if (m_Net)
            {
                if (m_Net->Server)
                {
                    m_Net->Server->DeliverMessages();
                }
                if (m_Net->ClientHost)
                {
                    m_Net->ClientHost->DeliverMessages();
                }
            }
        }

        // Before BeginFrame: continuations that register or retire resources must
        // land before AcquireNextFrame or their GPU-state mutation is frame-ambiguous.
        // PumpMainThread carries its own scope.
        m_TaskSystem->PumpMainThread();

        // Drive the audio subsystem's main-thread half: publish the current mix state, advance the
        // null device's virtual clock, and reap finished voices and reclaimed resources. A real
        // device mixes on its own callback thread.
        if (m_AudioDevice)
        {
            VE_PROFILE_SCOPE("Frame/AudioPump");
            m_AudioDevice->Pump(delta);
        }

        // Finalize resident async loads (bindless registration + cache swap) before
        // BeginFrame, in the same main-thread window as the continuation pump.
        {
            VE_PROFILE_SCOPE("Frame/AssetFinalize");
            m_AssetManager->PumpFinalizes();
        }

        // The managed world's interpolation fraction drives the view pushes and a game's overlay Update;
        // resolve it before the tick so its alpha is read after the advance.
        World* const managed = m_WorldRunner->ResolveWorld(m_ManagedWorld);

        // Client tick-offset control, per net-active client world: each runs its sim tick ahead of the
        // server so the input it stamps for a tick arrives before the server's scheduled consume reaches
        // it. Each world's tick-offset controller (keyed by its JoinId) is the single source of truth for
        // how far ahead the world runs; this seeds the world's epoch to it and slews its sim step toward
        // it (the world's Slew, applied as that world's SimScale). Reset every client world's slew first,
        // so a world not yet syncing (no snapshot, or paused) runs unscaled.
        if (m_Net && m_Net->ClientHost)
        {
            for (auto& [worldValue, clientWorld] : m_Net->ClientWorlds)
            {
                clientWorld.Slew = 1.0f;
            }
            for (const auto& [worldValue, role] : m_Net->WorldRoles)
            {
                if (role != NetRole::Client)
                {
                    continue;
                }
                const WorldInstanceId world{.Value = worldValue};
                World* const w = m_WorldRunner->ResolveWorld(world);
                const bool active = w != nullptr && w->GetScene().GetSimulation() != nullptr &&
                                    w->GetScene().GetSimulation()->IsStarted() && !w->IsPaused();
                const Net::JoinId join = ClientJoinForWorld(world);
                if (!active || join == Net::ControlJoinId ||
                    m_Net->ClientHost->LastServerTick(join) == 0)
                {
                    continue;
                }

                NetState::ClientWorldState& state = m_Net->ClientWorlds[world.Value];
                SimClock& clock = w->Clock;

                // Fold this frame's link state into the controller first, so the target lead it computes
                // drives both the hard snap and the bounded slew below — one target, never two
                // disagreeing ones (a seed to one lead that the slew then drags off).
                state.Slew = m_Net->ClientHost->ObserveTickSync(join, clock.GetTick());

                const i64 targetLead = static_cast<i64>(
                    std::lround(std::max(0.0f, m_Net->ClientHost->TickSync(join).TargetOffset())));
                const u64 desired =
                    m_Net->ClientHost->LastServerTick(join) + static_cast<u64>(targetLead);
                const i64 drift = static_cast<i64>(clock.GetTick()) - static_cast<i64>(desired);

                // Seed the epoch once — each SimClock starts at 0 when its process does, so a client
                // joining a long-running server must jump its tick to the server's or its input lands on
                // numbers the server's scheduled consume never matches. Thereafter hard-snap only when
                // the drift is too large for the bounded ±slew to claw back in time — a long hitch, a
                // step change in RTT, a spiral-clamp tick drop. A snap clears the now-stale prediction
                // history, so it is reserved for large drift and the slew handles the rest; the trigger
                // is drift itself, not the frame budget, so a client at a healthy frame rate still
                // recovers from a transient stall.
                const bool seed = !state.ClockSeeded;
                const bool largeDrift =
                    std::llabs(drift) > static_cast<i64>(m_Net->Info.SnapshotIntervalTicks + 6);
                if (seed || largeDrift)
                {
                    clock.SetTick(desired);
                    m_Net->ClientHost->History(join)
                        .Clear(); // captures on the pre-snap epoch are stale
                    state.ClockSeeded = true;
                    state.Slew = 1.0f; // fresh epoch: no residual to chase this frame
                    Log::Info("Client sim clock {} to tick {} (server {} + lead {})",
                              seed ? "seeded" : "re-synced", desired,
                              m_Net->ClientHost->LastServerTick(join), targetLead);
                }
            }
        }

        {
            VE_PROFILE_SCOPE("Frame/Input");

            // Roll the input snapshot forward, then poll the window and route this frame's events
            // through the router (folding into the snapshot, forwarding to ImGui by focus). Headless
            // borrows no window, so no events arrive and the snapshot stays neutral. The roll is held
            // only when the previous frame latched (an active sim ran no tick), so a pressed edge on a
            // zero-tick frame survives to the next tick-running frame; a frame with no active sim (the
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

            // Release one paced segment of any queued synthetic input (MCP/script injection) at the
            // same pre-tick point real window events land, so an injected event folds into this
            // frame's snapshot for the tick loop rather than after it (DrainInjectedEvents).
            m_InputRouter->DrainInjectedEvents();
        }

        // After the events are forwarded: ImGui's NewFrame consumes them this frame.
        if (m_ImGuiLayer)
        {
            VE_PROFILE_SCOPE("Frame/ImGui");
            m_ImGuiLayer->BeginFrame();
        }

        // A dedicated server (headless with a live host) runs the accumulator + net pump with no
        // View/render tail: the View systems are client-local presentation a headless process has no
        // consumer for. A windowed (listen) server keeps them for its local seats.
        const bool dedicatedServer = m_Info.Headless && GetServerHost() != nullptr;

        // Whether this frame records a render tail at all. A dedicated server never does — it has no
        // client-local presentation. A headless run that declared ApplicationInfo::HeadlessRendering
        // false does not either: its frames reach no consumer, so the capture, viewport, and composite
        // recording is work for nothing. It keeps the View phase and the view pushes, so the viewport
        // bindings and presentation state stay exactly as a rendering run leaves them.
        const bool renderTail = !dedicatedServer && (!m_Info.Headless || m_Info.HeadlessRendering);

        // Drive every world through the runner in id order: each world's fixed Sim steps (0..N, its
        // own accumulator's step count) then one View pass with its interpolation alpha. The pointer
        // routing is scoped to one scene, so a world's Sim gets it only when the pointer's owning
        // viewport presents that world's scene — a scene-local Owner handle never leaks across. The
        // managed world's Sim steps thread the net input feed: server-side each tick's change tick
        // keys replication dirty state and the buffered wire input fills each seat before the systems
        // run; client-side the local seat's resolved input is stamped after the systems.
        const ScopedPointer scoped = ComputePointerRouting();
        const WorldTickResult ticked = m_WorldRunner->Tick(WorldTickInfo{
            .Delta = delta,
            .RunViewPhase = !dedicatedServer,
            .BuildContext =
                [this, &scoped](const WorldInstanceId world, const Scene& scene, const u64 tick,
                                const f32 alpha, const bool firstStep)
            {
                const PointerRouting pointer =
                    &scene == scoped.Scene ? scoped.Routing : PointerRouting{};
                return BuildSystemContext(scene, RoleForWorld(world), pointer, tick, alpha,
                                          firstStep);
            },
            .SimScale = [this](const WorldInstanceId world) -> f32
            {
                if (m_Net)
                {
                    const auto it = m_Net->ClientWorlds.find(world.Value);
                    if (it != m_Net->ClientWorlds.end())
                    {
                        return it->second.Slew;
                    }
                }
                return 1.0f;
            },
            .BeforeSimStep =
                [this, &scoped](const WorldInstanceId world, Scene& scene, const u64 tick)
            {
                // Latch the pointer motion accumulated since this world's previous step as the step's
                // delta, so a seat resolving at the fixed rate reads the motion since the last tick
                // rather than since the last frame. The accumulation is one shared pointer stream and
                // the call consumes it, so only the routed scene's steps take it: another world's
                // seats read neutral mouse anyway (SeatInputView::OwnsPointer), and draining on their
                // behalf would leave the routed world nothing. With no scene routed nothing can read
                // the mouse, so every step drains — that keeps an unrouted stretch from banking.
                if (scoped.Scene == nullptr || &scene == scoped.Scene)
                {
                    m_Input->BeginSimTick();
                }

                if (IsWorldNetActive(world) && RoleForWorld(world) == NetRole::Server)
                {
                    scene.SetChangeTick(tick);
                    FeedServerSeatInputs(world, tick);
                }
            },
            .AfterSimStep =
                [this](const WorldInstanceId world, Scene&, const u64 tick)
            {
                if (IsWorldNetActive(world) && RoleForWorld(world) == NetRole::Client)
                {
                    const Net::JoinId join = ClientJoinForWorld(world);
                    if (join != Net::ControlJoinId)
                    {
                        // The Sim phase just ran control + movement for the Predicted set (the authority
                        // filter answers true for it client-side); stamp the seat input to send, then
                        // record this tick's input and predicted state for this world's reconciliation.
                        StampClientInput(world, tick);
                        m_Net->ClientHost->RecordPrediction(join, tick);
                    }
                }
            },
        });

        // The managed world's interpolation fraction drives the view pushes and a game's overlay Update.
        m_SimAlpha = managed != nullptr ? managed->LastAlpha : 0.0f;

        // Receive the join/state stream and flush this frame's sends after the ticks: server-side the
        // snapshot the host generates reflects this frame's just-ticked state (keyed to the last tick),
        // client-side the stamped input window is sent. Placed after the tick loop so a snapshot's
        // content and its tick label agree.
        PumpNet();

        // Reconcile the locally-controlled markers after the ticks and the net pump, so a possession
        // granted in this frame's Sim and one that arrived in this frame's snapshot are both marked
        // before the frame renders — and each move raises OnClientPossession, in every mode.
        SyncLocalControl();

        // The edge latch: a frame with a live world that ran no tick holds its edges for the next
        // tick-running frame; a frame with no active world never latches (it rolls next frame).
        m_PreviousFrameLatchedInput = ticked.AnyActive && !ticked.AnyTicked;

        // The Sim-delta accumulation follows the same distinction. A frame that ran no tick under a
        // live world is mid-accumulation and holds its motion for the tick-running frame to come; a
        // frame with nothing simulating drops it, so a stopped or paused stretch's whole travel does
        // not arrive as the resuming tick's look. Mirrors the runner resetting an inactive world's
        // clock so resuming chases no backlog.
        if (!ticked.AnyActive)
        {
            m_Input->DropSimDeltas();
        }

        {
            VE_PROFILE_SCOPE("Frame/Update");
            OnUpdate(delta);
        }

        // Pull each managed viewport's camera from the world it names and push it: a viewport naming a
        // Viewer gets that seat's camera resolved at its aspect, otherwise the world's scene primary
        // camera; a viewport whose world was closed renders a cleared target; one with no bound world
        // is left for the game to drive. Plus the per-frame view knobs. After OnUpdate so a game's
        // per-frame scene edits are reflected; before the viewport render phase reads it. A dedicated
        // server pushes nothing — it has no render tail.
        if (!dedicatedServer)
        {
            VE_PROFILE_SCOPE("Frame/ViewPush");
            m_ManagedViewports->PushViews(*m_WorldRunner, m_WorldView, delta, m_SimAlpha);
        }

        {
            VE_PROFILE_SCOPE("Frame/RenderBegin");
            m_RenderContext.BeginFrame();
        }

        Renderer::CommandBuffer& cmd = m_RenderContext.GetCurrentCommandBuffer();

        // A frameless run ends the frame here: the accumulator, the net pump, and (outside a dedicated
        // server) the View phase have run, and no consumer reads what the tail would record.
        if (!renderTail)
        {
            {
                VE_PROFILE_SCOPE("Frame/RenderEnd");
                m_RenderContext.EndFrame();
            }
            BridgeGpuTimings();
            return;
        }

        // Build, register, and push this frame's source into every presented world's authored capture
        // surfaces, so a scene-declared capture joins the drive-list beside any imperatively-registered
        // ones. A world no view shows drives none: its captures could not be sampled, and several live
        // worlds at once is ordinary (make-before-break travel plus a keep-warm dwell), so the drive
        // would otherwise scale the frame's view budget by the number of worlds held warm.
        m_WorldRunner->DriveCaptureSurfaces({
            .Register = [this](Renderer::SceneCapture& capture) { RegisterCapture(capture); },
            .IsPresented = [this](const WorldInstanceId world) { return IsWorldPresented(world); },
        });

        // The engine render phase, uniform for every app and not overridable. The compositor
        // renders every registered capture first (so a material sampling a capture's output reads
        // this frame's result), then every registered viewport in registration order — each into
        // Sample layout, so viewport outputs are sampleable before OnRender builds the ImGui draw
        // data that may sample them.
        {
            VE_PROFILE_SCOPE("Frame/Render");
            m_Compositor.RenderRegistered(cmd);
        }

        // The app builds its ImGui frame and records any extra draws; it no longer runs the
        // composite or ImGuiLayer::Render — those bracket it in the engine phase.
        {
            VE_PROFILE_SCOPE("Frame/OnRender");
            OnRender();
        }

        // When ImGui is on, record the overlay then composite the Presented viewports behind it.
        if (m_ImGuiLayer)
        {
            VE_PROFILE_SCOPE("Frame/Composite");
            m_ImGuiLayer->Render(cmd);
            m_Compositor.Composite(cmd);
        }

        // Sample the renderer's cull-funnel and point-field counters once per frame, at the end of
        // the render block; the getters keep their ownership, this only puts them on a timeline.
        SampleRenderCounters();

        {
            VE_PROFILE_SCOPE("Frame/RenderEnd");
            m_RenderContext.EndFrame();
        }

        // Lift the last completed frame's GPU pass timings onto the virtual GPU track, back-dated to
        // the frame that executed them.
        BridgeGpuTimings();
    }

    void Application::SampleFrameCounters()
    {
        if (m_TaskSystem)
        {
            VE_PROFILE_COUNTER("TaskSystem/QueueDepth",
                               static_cast<f64>(m_TaskSystem->GetQueueDepth()));
            VE_PROFILE_COUNTER("TaskSystem/ActiveJobs",
                               static_cast<f64>(m_TaskSystem->GetActiveJobCount()));
            VE_PROFILE_COUNTER("TaskSystem/MainThreadQueue",
                               static_cast<f64>(m_TaskSystem->GetMainThreadQueueDepth()));
        }
    }

    void Application::SampleRenderCounters()
    {
#if defined(VE_PROFILE) && VE_PROFILE
        if (m_ManagedViewports == nullptr || m_ManagedViewports->GetCount() == 0)
        {
            return;
        }
        const Renderer::Viewport* viewport = m_ManagedViewports->Get(0);
        if (viewport == nullptr)
        {
            return;
        }
        const Renderer::SceneRenderer& renderer = viewport->GetRenderer();

        // The cull funnel, read as four aligned series: visible → frustum-survived → GPU-survived →
        // drawn. The getters keep their ownership; this adds the sampler.
        VE_PROFILE_COUNTER("Render/Visible", static_cast<f64>(renderer.GetLastVisibleCount()));
        VE_PROFILE_COUNTER("Render/FrustumSurvived",
                           static_cast<f64>(renderer.GetFrustumSurvivedCount()));
        VE_PROFILE_COUNTER("Render/GpuSurvivors",
                           static_cast<f64>(renderer.GetLastGpuSurvivorCount()));
        VE_PROFILE_COUNTER("Render/Drawn", static_cast<f64>(renderer.GetLastDrawnCount()));

        const Renderer::PointFieldStats pointFields = renderer.GetPointFieldStats();
        VE_PROFILE_COUNTER("Render/PointFieldCells", static_cast<f64>(pointFields.CellsInFrustum));
        VE_PROFILE_COUNTER("Render/PointFieldSprites", static_cast<f64>(pointFields.SpritePoints));
        VE_PROFILE_COUNTER("Render/PointFieldSplats", static_cast<f64>(pointFields.Splats));
#endif
    }

    void Application::BridgeGpuTimings()
    {
#if defined(VE_PROFILE) && VE_PROFILE
        const Renderer::Context& context = m_RenderContext;

        // Timing unsupported → the GPU track is simply absent, not empty-and-broken.
        if (!context.IsGpuTimingSupported())
        {
            return;
        }

        constexpr u64 NoFrame = ~0ULL;
        const u32 slotCount = context.GetMaxFramesInFlight();
        if (m_GpuSlotFrame.size() != slotCount)
        {
            m_GpuSlotFrame.assign(slotCount, NoFrame);
            m_GpuSlotAnchorTicks.assign(slotCount, 0);
        }

        const u32 slot = context.GetCurrentFrameInFlight();

        // The timings now readable belong to the frame that last occupied this slot — N frames ago,
        // remembered when it was submitted. Stamp the events with that frame, never the current one.
        const u64 readbackFrame = m_GpuSlotFrame[slot];
        const u64 anchorTicks = m_GpuSlotAnchorTicks[slot];

        const std::span<const Renderer::Context::GpuPassTiming> timings =
            context.GetLastGpuPassTimings();
        if (readbackFrame != NoFrame && !timings.empty())
        {
            if (m_GpuTrack == 0)
            {
                m_GpuTrack = m_Profiler.CreateTrack("GPU", Diagnostics::TrackRole::Gpu);
            }

            const u64 frequency = Diagnostics::TraceTickFrequency();
            const auto nanosToTicks = [frequency](const u64 nanos)
            {
                return static_cast<u64>((static_cast<f64>(nanos) * static_cast<f64>(frequency)) /
                                        1.0e9);
            };

            // The enclosing whole-frame GPU scope, so the track flamegraphs like a CPU one.
            const u64 frameNanos = static_cast<u64>(context.GetLastGpuFrameTimeMs() * 1.0e6f);
            m_Profiler.EmitScope(m_GpuTrack, m_Profiler.InternName("GPU Frame"), anchorTicks,
                                 anchorTicks + nanosToTicks(frameNanos), readbackFrame);

            // Each pass placed by its begin/end ticks; nesting falls out of tick containment (the
            // depth the accessor now carries reconstructs the same tree).
            for (const Renderer::Context::GpuPassTiming& pass : timings)
            {
                m_Profiler.EmitScope(m_GpuTrack, m_Profiler.InternName(pass.Name),
                                     anchorTicks + nanosToTicks(pass.BeginNanos),
                                     anchorTicks + nanosToTicks(pass.EndNanos), readbackFrame);
            }
        }

        // This frame's GPU work now occupies the slot (submitted in the EndFrame just run); remember
        // the frame index it was stamped with and a trace-clock anchor for its GPU start.
        m_GpuSlotFrame[slot] = m_Profiler.GetFrameIndex();
        m_GpuSlotAnchorTicks[slot] = Diagnostics::NowTicks();
#endif
    }
}
