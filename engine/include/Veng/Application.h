#pragma once

#include <Veng/Veng.h>
#include <Veng/Assert.h>
#include <Veng/LaunchArguments.h>
#include <Veng/Window.h>
#include <Veng/Input.h>
#include <Veng/InputRouter.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Level.h>
#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/SceneCapture.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Renderer/ViewportCompositor.h>
#include <Veng/Renderer/GatherPass.h>
#include <Veng/Renderer/RenderGraph.h>
#include <Veng/Renderer/SwapChainCompositePass.h>
#include <Veng/ImGui/ImGuiLayer.h>
#include <Veng/Gui/GuiConsumer.h>
#include <Veng/Net/AccountId.h>
#include <Veng/Net/Host.h>
#include <Veng/Net/Interest.h>
#include <Veng/Net/JoinRequest.h>
#include <Veng/Net/PredictionHistory.h>
#include <Veng/Net/Session.h>
#include <Veng/Net/Blob.h>
#include <Veng/Task/TaskSystem.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SimClock.h>
#include <Veng/Scene/SystemRegistry.h>
#include <Veng/World.h>
#include <Veng/WorldRunner.h>
#include <Veng/ManagedViewports.h>
#include <Veng/WorldDirectory.h>

#include <span>
#include <unordered_map>
#include <unordered_set>

namespace Veng
{
    class ServerHost;
    class ClientHost;
    class GuiDriverRegistry;
    namespace Net
    {
        class Client;
    }

    /// @brief Returns the directory containing the running executable.
    ///
    /// A game mounts its asset pack relative to this so the launcher + module +
    /// pack resolve beside the binary, not at an absolute build-tree path.
    /// @return Absolute path to the directory holding the running binary.
    [[nodiscard]] VE_API path ExecutableDirectory();

    /// @brief Opt-in configuration for the engine-managed game world.
    ///
    /// Set ApplicationInfo::World to this to have Application bootstrap and drive the running
    /// game: it reads the cooked project file beside the executable, mounts each pack it names,
    /// loads the project's startup level, opens it through the WorldRunner as world #0 (a Scene with
    /// the level's SceneSimulation attached), seeds the renderer from the level's render settings,
    /// binds world #0 to managed viewport #0, and each frame ticks every world and pushes each
    /// viewport's resolved camera. A game reaches the running world by handle
    /// (GetWorldRunner().ResolveWorld(GetManagedWorldId())->GetScene()) and customizes it in
    /// OnWorldLoaded; the minimal game needs no code at all. Requires ManagedViewport to be set (the
    /// world renders through the managed viewport).
    struct GameWorldInfo
    {
        /// @brief Cooked project file to bootstrap from, resolved relative to the executable.
        ///
        /// Read from ExecutableDirectory() / Project; it names the packs to mount (each resolved
        /// beside the executable too) and the startup level the engine loads and runs.
        path Project;
        /// @brief Fixed simulation ticks per second the Sim phase steps at (the accumulator rate).
        ///
        /// The world drive accumulates frame time and steps the Sim phase at this fixed rate with a
        /// monotonic tick number, decoupling simulation from the frame rate; the View phase and render
        /// still run per frame, interpolating between the last two ticks. Must be positive.
        u32 SimTickRate = 60;
        /// @brief Whether the bootstrap restores the local account's session once world #0 is bound.
        ///
        /// True (the default) is the continue-style posture: the bootstrap consults the local
        /// account's session record and, when it carries a gameplay world, present-on-ready rebinds
        /// managed viewport 0 onto it — a saved sitting resumes with no game code. False suppresses
        /// that entirely: world #0 (the startup level) stays presented and the game decides when to
        /// restore, calling Application::RestoreLocalSession itself once it has opened the store the
        /// record lives in — the posture a front-end that owns the first travel, or a player-less
        /// dedicated host with no local account, wants. The restore path is identical either way.
        bool RestoreLocalSessionOnBoot = true;
    };

    /// @brief Opt-in networking knobs for the engine-managed world; activation is a launch decision.
    ///
    /// Set ApplicationInfo::Net to tune the hosts the engine mounts when the launcher activates a net
    /// mode (`--server` opens a ServerHost on the managed world; `--join` connects a ClientHost). The
    /// knobs are only knobs — a game that leaves Net unset still gets these defaults when launched
    /// `--server`, so zero-config LAN hosting works with no configuration. A default (no net launch
    /// flag) constructs no host and behaves exactly as an offline app.
    struct GameNetInfo
    {
        /// @brief UDP port the server listens on, and the default a `--join` with no `:port` uses.
        u16 Port = 27750;
        /// @brief Maximum simultaneously accepted connections; a further request is denied.
        u32 MaxConnections = 16;
        /// @brief Emit a snapshot every this many sim ticks (2 ⇒ 30 Hz at a 60 Hz sim).
        u32 SnapshotIntervalTicks = 2;
        /// @brief How many recent input ticks each client input packet carries redundantly (the loss window).
        u32 InputRedundancyTicks = 3;
        /// @brief Whether the server quantizes Transform's spatial leaves on the wire (lossy, wire-only).
        ///
        /// On by default: a displayed pose does not need full f32 precision, so the snapshot wire
        /// rounds position to PositionQuantum and rotation to smallest-three. The sim state stays
        /// full-float on both ends; only the wire representation rounds. Reconciliation's spatial
        /// epsilon is kept >= the quantum so the rounding never reads as a misprediction.
        bool QuantizeSpatial = true;
        /// @brief Position grid step in meters the wire quantizes to (the max round error is half this).
        f32 PositionQuantum = 0.001f;
        /// @brief Half the encodable world span per axis in meters; a position past it clamps.
        f32 PositionExtent = 4096.0f;
        /// @brief Bits per smallest-three rotation component on the wire.
        u32 RotationBits = 9;
        /// @brief Force a full (non-delta) snapshot record every this many snapshots per connection.
        u32 KeyframeIntervalSnapshots = 16;
        /// @brief Interest radius in meters around a connection's pawn; 0 (the default) replicates the whole world.
        ///
        /// When positive, a connection hears only about the entities within this radius of its pawn,
        /// plus the always-relevant marks (the seats, a game's global state) and the InterestPolicy hook — the
        /// scale lever that stops bandwidth growing with world size. Zero is the planset-54 behavior,
        /// so interest is opt-in per game.
        f32 InterestRadius = 0.0f;
        /// @brief The interest boundary hysteresis: the leave radius is InterestRadius times this.
        f32 InterestLeaveMultiplier = 1.15f;
        /// @brief The minimum snapshots an entity stays in a connection's interest set after entering.
        u32 InterestMinDwellSnapshots = 4;
        /// @brief Game hook adding entities to a connection's interest set beyond the spatial query; unset adds none.
        Net::InterestPolicy InterestPolicy;
        /// @brief Client policy selecting the predicted entity set on a possession change; null uses the default.
        ///
        /// The engine passes it to the mounted ClientHost, which promotes this set to Tier::Predicted
        /// and re-runs the real Sim systems for it client-side each tick. Unset uses the
        /// owner-pawn-subtree default (the pawn plus its replicated attachments); a game widens it (a
        /// driven vehicle) or narrows it here. Inert off a client.
        PredictionPolicy PredictionPolicy;
        /// @brief Client hook yielding the content digest the client expects for a joined WorldKey; unset validates against none.
        ///
        /// The client mirror of the server's per-key ServerWorldResolution::Digest: given a WorldKey the
        /// client is joining and the join reply's echoed opaque travel payload, returns the digest of
        /// its own procedurally-reconstructed world, which the mounted ClientHost compares against the
        /// join reply's echoed digest — a mismatch rejects the join loudly. A per-key provider (a
        /// connection may join several worlds by opaque key); a world parameterized by payload rather
        /// than key folds the echoed payload in. Unset presents the zero digest (matching a
        /// content-free server world), the zero-config default. Inert off a client.
        function<Net::ContentDigest(const Net::WorldKey&, const Net::Blob&)> ClientWorldDigest;
        /// @brief Client hook yielding the spatial dequantization grid a joined WorldKey decodes with; unset uses the shared envelope.
        ///
        /// The client mirror of the server's per-world ServerWorldResolution::Replication quantization:
        /// given a WorldKey the client is joining, returns the dequantization grid that join decodes
        /// with, so two hosted worlds with different spatial envelopes both decode correctly on one
        /// client. Unset threads the shared PositionQuantum/PositionExtent/RotationBits onto every join.
        /// Inert off a client.
        function<Net::QuantizationSettings(const Net::WorldKey&)> ClientWorldQuantization;
        /// @brief Whether a joining client auto-joins Net::DefaultWorldKey into the managed world on connect.
        ///
        /// True (the default) is the single-world convenience: the mounted ClientHost requests
        /// Net::DefaultWorldKey the moment it connects, and its reply loads into the managed world #0 —
        /// byte-identical to the single-join behavior. False suppresses the auto-join: Connect (or a
        /// `--join` launch) stands up the transport and ClientHost without joining any world, leaving the
        /// managed world untouched, so a consumer explicitly joins the WorldKey(s) it wants through
        /// Application::JoinWorld — each landing in its own runner world. Inert off a client.
        bool AutoJoinDefaultWorld = true;

        /// @brief The local player's account identity; unset mints a process-random ephemeral id.
        ///
        /// Evaluated once per process activation — the standalone bootstrap, a `--join` launch, or a
        /// runtime Connect — to resolve the account the process plays as: a client presents it at the
        /// handshake, and a standalone or listen host registers it into directory presence per its
        /// joins, so single-player personal worlds and saves key identically to multiplayer. The
        /// engine never interprets the returned id (a consumer packs a config identity, a
        /// machine-derived id, an auth-token subject). Unset mints a random valid id — reattach and
        /// persistence then key on nothing durable across relaunches (the zero-config LAN posture). A
        /// headless dedicated launch (`--dedicated`) never evaluates it — the host is nobody.
        ///
        /// @warning Whoever presents an account id *is* that account (see Net::AccountId): hosting
        ///          beyond a trusted LAN is unsafe until AdmitAccount verifies identity.
        function<Net::AccountId()> Identity;
        /// @brief The local account's opaque profile, presented at admission; unset presents none.
        ///
        /// The sanctioned channel for account-level game data at admission. Evaluated once beside
        /// Identity, per process activation: a client presents the blob in its connect request, and
        /// a listen host or standalone app binds the identical value for its own account, so
        /// ServerHost::ProfileOf and Net::JoinRequestInfo::Profile answer the same in all three
        /// topologies. The engine transports and holds the bytes but **never decodes them**, never
        /// replicates them, and never forwards them to another peer — a game wanting peer-visible
        /// identity replicates its own component instead.
        ///
        /// The blob must encode to at most Net::MaxProfileBytes: the connect request is one
        /// unfragmented reliable message, so a larger profile refuses the connect with
        /// Net::DenyReason::ProfileTooLarge rather than being truncated.
        ///
        /// @warning The profile is client-authored data under the existing admission trust posture
        ///          (see Net::AccountId): a host may assert what it says, never verify it.
        function<Net::Blob()> PresentProfile;
        /// @brief Server hook admitting or normalizing a presented account; unset accepts as presented.
        ///
        /// Threaded onto the mounted server's handshake: called with the connection id being assigned
        /// and the account the client presented; the returned id is the one bound to the connection.
        /// Returning nullopt refuses the connection with the "account refused" deny reason. This is
        /// where an authentication layer verifies a token — and where a host can wire an allowlist
        /// today as a stopgap; the unset default trusts the presented id exactly as LAN play trusts
        /// the presented connection. A duplicate live account is refused regardless (see
        /// Net::DenyReason::AccountAlreadyConnected — retryable across the disconnect-timeout window).
        /// Inert off a host.
        function<optional<Net::AccountId>(Net::ConnectionId, const Net::AccountId&)> AdmitAccount;

        /// @brief Rewrites an account's session record as its reattach begins; unset restores it as recorded.
        ///
        /// The game's one word on reconnect placement (resurface in a different regime, veto a
        /// stale location): the returned record is what the reattach restores and what the registry
        /// keeps. Threaded into the host-tier session registry; see
        /// Net::SessionRegistryInfo::TransformOnReattach.
        function<Net::SessionRecord(Net::SessionRecord)> TransformOnReattach;
        /// @brief Encodes an account's current gameplay pose from its seat; unset keeps the last travel's.
        ///
        /// The engine cannot serialize game pose, so the game encodes it: invoked at disconnect and
        /// at the save checkpoint with the account's gameplay world and its seat entity there, and
        /// the result is delivered back on reattach. See Net::SessionRegistryInfo::CaptureTravelPose.
        function<Net::Blob(WorldInstanceId, Entity)> CaptureTravelPose;
        /// @brief Loads an account's persisted session blob on first admit; unset keeps records process-lifetime.
        ///
        /// The durability hook pair's read half: the engine owns when (first admission) and what
        /// (the record's reflection-binary encoding), the game owns where. See
        /// Net::SessionRegistryInfo::LoadSession.
        function<optional<vector<std::byte>>(Net::AccountId)> LoadSession;
        /// @brief Persists an account's session blob; unset keeps records process-lifetime.
        ///
        /// The durability hook pair's write half: invoked on disconnect, on StopNet/teardown, and
        /// debounced at the checkpoint. See Net::SessionRegistryInfo::SaveSession.
        function<void(Net::AccountId, std::span<const std::byte>)> SaveSession;

        /// @brief The get-or-place world factory the mounted ServerHost resolves a joined WorldKey through.
        ///
        /// Materializes a world for a key that has no live bucket (opening a scene through the game's own
        /// runner and returning it) so a client may join a world by content, not only the pre-registered
        /// managed world. Unset means only the managed world is joinable (the single-world default). Read
        /// by both the `--server` launch path and the runtime StartHosting call; inert off a host. The
        /// requesting JoinRequestInfo rides in ahead of the key and payload (which it also carries), so a
        /// world can project the requester's account at open — per-account state for the joining player.
        /// A resolve not driven by a particular join (an Application::HoldWorldWarm pre-open) passes a
        /// requester-less request: the invalid account, which a factory treats as "no specific requester".
        function<optional<ServerWorldResolution>(const Net::JoinRequestInfo&, const Net::WorldKey&,
                                                 const Net::Blob&)>
            WorldFactory;
        /// @brief The authorization hook: may this requester join or create this key? Unset allows all.
        ///
        /// Threaded into the world directory, called before any world open or JoinId assignment with
        /// the request identity (connection, account, key, payload) — a policy may gate on who is
        /// asking or on arrival data. A standalone travel authorizes with ConnectionId{} and the
        /// local account; a connection-borne join carries the admitted account.
        function<bool(const Net::JoinRequestInfo&)> Authorize;
        /// @brief Closes a factory-opened world when it idles out; unset leaves the world's runner teardown alone.
        ///
        /// The counterpart to WorldFactory: invoked with a factory-opened world's id once it has been
        /// presence-less past the idle keep-warm dwell — before the directory's runner teardown, so a
        /// game captures its persistent state here.
        function<void(WorldInstanceId)> CloseWorld;
        /// @brief The get-or-place policy for a WorldKey's instances; unset uses the capacity policy.
        ///
        /// Threaded into the world directory, called with the request identity (connection, account,
        /// key, payload) and the key's live buckets. Unset selects the built-in capacity policy driven
        /// by MaxPlayersPerInstance; a game supplies its own for a different fill rule (a proximity
        /// match comparing the requester's payload against each live bucket's recorded params).
        function<optional<WorldInstanceId>(const Net::JoinRequestInfo&,
                                           std::span<const WorldPlacement>)>
            Placement;
        /// @brief Per-instance seat cap the built-in placement policy buckets a key to; 0 = no cap (convergence).
        ///
        /// 0 (the default) converges every joiner of a key on one instance; a value > 0 buckets a busy
        /// key into instances of at most this many seats, spun up on demand through WorldFactory. Ignored
        /// when Placement is set.
        u32 MaxPlayersPerInstance = 0;
        /// @brief The server-wide bound on total live hosted worlds; a fresh-bucket open past it is denied.
        u32 MaxHostedWorlds = 64;
        /// @brief The most worlds one connection may join before a further join is denied.
        ///
        /// The default budgets the standing-join architecture the engine itself recommends: one
        /// presenting gameplay world, plus the standing data worlds an account typically holds
        /// across reconnects — a per-account world, a shared state-projection world, a group
        /// world — is four concurrent joins, and a make-before-break travel overlaps a fifth while
        /// the destination readies. Eight leaves headroom for another standing data world and a
        /// second in-flight transition while still capping join fan-out abuse.
        u32 MaxJoinedWorldsPerConnection = 8;
        /// @brief Seconds a factory-opened world with no live joins is held warm before it is reaped.
        f64 IdleKeepWarmDwell = 5.0;
    };

    /// @brief Construction parameters for Application.
    struct ApplicationInfo
    {
        /// @brief Application name passed to the Vulkan instance.
        string Name = "Veng Application";
        /// @brief Engine name passed to the Vulkan instance.
        string EngineName = "Veng";
        /// @brief Off-screen render-target extent used only when Headless.
        ///
        /// Headless runs borrow no window, so there is no swapchain to derive a render-target
        /// size from; this is the extent the render target (and the managed viewport) takes.
        /// Ignored windowed, where the swapchain framebuffer extent drives it instead.
        uvec2 HeadlessExtent{1280, 720};
        /// @brief Window creation parameters.
        WindowInfo WindowInfo;
        /// @brief ImGui integration; nullopt disables it for UI-free apps.
        ///
        /// Engaged by default. Force-disabled when Headless (ImGui requires a window).
        optional<ImGuiLayerInfo> ImGui = ImGuiLayerInfo{};
        /// @brief Run without a window, using an off-screen context; exits on RequestExit().
        bool Headless = false;
        /// @brief Whether a Headless run records the per-frame render tail; ignored windowed.
        ///
        /// A headless run still builds the whole render path and renders every registered capture
        /// and viewport into its off-screen target, so a consumer that reads frames back (an
        /// image-comparison harness, a smoke run) gets them. A consumer that observes no frame pays
        /// that cost for output nobody reads — on a validation-layered debug build enough of the
        /// frame budget to starve the fixed-step and network pumps that share the loop. Clearing
        /// this drops the capture, viewport, and composite recording; the simulation, the View
        /// phase, and the net pump are untouched, so a frameless client still ticks and converges.
        /// The engine already infers this for a dedicated server (Headless with a live ServerHost),
        /// which additionally has no client-local presentation to drive; this is the declaration for
        /// every other frameless run. `--no-render` clears it from the command line.
        bool HeadlessRendering = true;
        /// @brief Requested display output mode for the swapchain (a preference; see DisplayMode).
        ///
        /// Defaults to picking the best available HDR mode, falling back to SDR. The resolved
        /// result is read back via Context::GetActiveDisplayMode().
        Renderer::DisplayMode RequestedDisplayMode = Renderer::DisplayMode::Auto;
        /// @brief Path for pipeline cache persistence; nullopt keeps the cache in-memory only.
        ///
        /// When set, seeds the pipeline cache from this file at startup (if it exists)
        /// and writes it back at shutdown. veng does not choose the path.
        optional<path> PipelineCachePath = std::nullopt;
        /// @brief Opt-in engine-owned managed primary viewport; nullopt leaves the app to own its views.
        ///
        /// When set, Application constructs one Presented viewport covering the window (default full
        /// Layout), registers it, tracks swapchain resize, and exposes it via GetManagedViewports().
        /// The plug-and-play path for a game. Convenience for a one-element ManagedViewports: when
        /// ManagedViewports is empty this becomes its sole entry. Unset (the editor) means the managed
        /// set is empty (GetManagedViewports().Get(0) returns null).
        optional<ManagedViewportInfo> ManagedViewport = std::nullopt;
        /// @brief Opt-in engine-owned managed viewport set; the multi-viewport form of ManagedViewport.
        ///
        /// When non-empty, this is the managed set the engine constructs, registers, and drives —
        /// each with its normalized Layout resolved to pixels per resize, index 0 the primary. The
        /// singular ManagedViewport is sugar for a one-element vector: if this is empty and
        /// ManagedViewport is set, that one info becomes the sole managed viewport. Runtime changes
        /// go through ReconfigureManagedViewports.
        vector<ManagedViewportInfo> ManagedViewports;
        /// @brief Opt-in engine-managed game world; nullopt leaves the app to load and drive its own.
        ///
        /// When set (and ManagedViewport is too), Application mounts the named pack, loads the
        /// pack's startup level, owns the running Scene + SceneSimulation, and ticks + pushes the
        /// view each frame. Unset means the app loads and drives its own world (the editor, or a
        /// game wanting full control).
        optional<GameWorldInfo> World = std::nullopt;
        /// @brief Networking knobs for the engine-managed world; nullopt uses the zero-config defaults.
        ///
        /// Tunes the hosts the engine mounts when the launcher activates a net mode (`--server` /
        /// `--join`). Purely knobs — activation is a launch decision, not this field, so a windowed
        /// game with Net unset still hosts on the defaults when launched `--server`. Offline (no net
        /// flag) it is inert. Requires World to be set (net drives the managed world).
        optional<GameNetInfo> Net = std::nullopt;
        /// @brief Command-line options this application accepts, surfaced in LaunchArguments::GameOptions.
        ///
        /// The launch parser consumes each declared option into GameOptions, which the application
        /// reads back through GetLaunchArguments(). An *undeclared* `--flag` remains a hard error, so
        /// a misspelled engine or application flag is caught rather than silently ignored. An engine
        /// flag of the same name wins; declaring nothing leaves parsing to the engine flags alone.
        vector<LaunchOptionInfo> LaunchOptions;
        /// @brief Host-owned asset-type identities merged into the AssetManager's own builtins.
        ///
        /// A module registers its own asset types into the host's registry through
        /// VengModuleHost::AssetTypes and points this at the same registry from its Application
        /// factory. Null (the default) means no module-defined asset types; the manager still
        /// knows every builtin, so a game registering none leaves this and AssetLoaders unset.
        ///
        /// Deliberately *not* pushed in by the launcher the way SetGuiDriverRegistry pushes the
        /// driver catalog: only a module that defines asset types needs these, and that module is
        /// already building the ApplicationInfo, so it has the registries in hand. A second
        /// setter would be a second way to say the same thing, with last-writer-wins between them.
        /// @warning Borrowed. The module handle must outlive the Application.
        const AssetTypeRegistry* AssetTypes = nullptr;
        /// @brief Host-owned AssetLoader factories the AssetManager instantiates at construction.
        ///
        /// The runtime half of a module-defined asset type: registered through
        /// VengModuleHost::AssetLoaders before any Context exists, instantiated once the manager
        /// is built. Null (the default) means no module-defined loaders.
        /// @warning Borrowed. The module handle must outlive the Application.
        const AssetLoaderRegistry* AssetLoaders = nullptr;
    };

    /// @brief The destination of an Application::Travel: the key, arrival payload, and presentation choice.
    struct TravelInfo
    {
        /// @brief The opaque world to travel to (the directory / server resolves it).
        Net::WorldKey Key;
        /// @brief Opaque arrival data threaded into the destination; empty is valid.
        Net::Blob Payload;
        /// @brief The managed viewport index that presents the destination.
        usize ViewportIndex = 0;
        /// @brief True to present the destination on the viewport; false resolves/joins without presenting (data worlds).
        bool Present = true;
        /// @brief Explicit standing choice for the session record; unset resolves to !Present.
        ///
        /// A presenting travel is the account's gameplay world, a non-presenting one a standing
        /// join restored on reattach; setting this overrides (false opts the travel out of the
        /// record entirely — a prefetch, a spectate). See Net::ResolveSessionDurability.
        optional<bool> Standing;
    };

    /// @brief Base class for a veng application; subclass and override the lifecycle hooks.
    class Application
    {
    public:
        /// @brief Constructs the application with the given settings and borrowed registries.
        ///
        /// The TypeRegistry and SystemRegistry are borrowed, not owned: the host (launcher
        /// or cooker) constructs them and fills them via VengModuleRegister before this
        /// runs. Both must outlive this Application.
        /// @param info     Application creation parameters.
        /// @param types    Host-owned registry of reflected types; must outlive the app.
        /// @param systems  Host-owned registry of scene systems; must outlive the app.
        Application(ApplicationInfo info, TypeRegistry& types, SystemRegistry& systems);

        /// @brief Destroys the application, tearing down the pimpl'd net state.
        ///
        /// Out-of-line so the net-state pimpl (an incomplete type in this header) is complete at the
        /// destruction site in the translation unit.
        virtual ~Application();

        /// @brief Enter the main loop, blocking until the app exits, and report the exit status.
        ///
        /// Returns the status set through RequestExit(i32) — 0 when the app never set one, so a run
        /// that simply ends reports success. The launcher returns this value from main, making a
        /// failed start distinguishable from a completed run to a supervisor or a script. Not
        /// [[nodiscard]]: a host that only needs the app to run may ignore the status.
        /// @param arguments  Command-line arguments forwarded from the launcher.
        /// @return The process exit status: 0 for a clean run, otherwise the requested status.
        i32 Run(vector<string> arguments);

        /// @brief Returns the application window.
        [[nodiscard]] Window& GetWindow() const { return *m_Window; }

        /// @brief Returns the launch arguments parsed from the command line at Run.
        ///
        /// Populated before Initialize; the engine already consumes the options it recognises
        /// (e.g. the startup-level override), so a game reads this only to inspect them itself.
        [[nodiscard]] const LaunchArguments& GetLaunchArguments() const { return m_LaunchArgs; }

        /// @brief Returns the frame-coherent input service.
        ///
        /// Always present, updated once per frame before OnUpdate/OnRender so per-frame
        /// edges and deltas reflect the current frame. A headless run reports the neutral
        /// all-zeros state rather than being absent.
        [[nodiscard]] Input& GetInput() const { return *m_Input; }

        /// @brief Returns the input router that routes window events to ImGui and the Input snapshot.
        ///
        /// Push InputFocus::Gameplay to give the running game exclusive input (and capture the
        /// cursor); pop it (or the Shift+Esc release chord) to return input to the UI. Always
        /// present; headless borrows no window and routes nothing.
        [[nodiscard]] InputRouter& GetInputRouter() const { return *m_InputRouter; }

        /// @brief Returns the render context.
        [[nodiscard]] Renderer::Context& GetRenderContext() { return m_RenderContext; }

        /// @brief Returns the task system.
        ///
        /// @pre Run() has initialized the engine — the task system exists only inside Run().
        [[nodiscard]] TaskSystem& GetTaskSystem()
        {
            VE_ASSERT(m_TaskSystem, "GetTaskSystem before Run(): the task system exists only once "
                                    "Run() has initialized the engine");
            return *m_TaskSystem;
        }

        /// @brief Returns the asset manager.
        ///
        /// @pre Run() has initialized the engine — the asset manager exists only inside Run().
        [[nodiscard]] AssetManager& GetAssetManager()
        {
            VE_ASSERT(m_AssetManager, "GetAssetManager before Run(): the asset manager exists only "
                                      "once Run() has initialized the engine");
            return *m_AssetManager;
        }

        /// @brief Returns the host-owned, process-wide registry of reflected types.
        ///
        /// Borrowed: the host constructs it, pre-registers builtins, and calls
        /// VengModuleRegister before passing it here. Must outlive this Application.
        [[nodiscard]] TypeRegistry& GetTypeRegistry() { return m_TypeRegistry; }

        /// @brief Returns the host-owned, process-wide registry of scene systems.
        ///
        /// Borrowed: the host constructs it and calls VengModuleRegister before passing
        /// it here, so a module's SceneSystem registrations are present. A SceneSimulation
        /// reads it to instantiate the running systems. Must outlive this Application.
        [[nodiscard]] SystemRegistry& GetSystemRegistry() { return m_SystemRegistry; }

        /// @brief Sets the host-owned GuiDriver catalog the engine drives claimed overlays through.
        ///
        /// The host (launcher/editor) calls VengModuleRegister — which fills this with the module's
        /// GuiDriver registrations — then hands the populated registry here before Run, so the managed
        /// viewports built during initialisation resolve each driven GuiOverlay's Driver id against it.
        /// Null (the default) leaves every overlay undriven. Borrowed; must outlive this Application.
        /// @param drivers  The host-owned driver catalog, or nullptr for none.
        void SetGuiDriverRegistry(GuiDriverRegistry* drivers) { m_GuiDriverRegistry = drivers; }

        /// @brief Returns the host-owned GuiDriver catalog, or nullptr when none was set.
        [[nodiscard]] GuiDriverRegistry* GetGuiDriverRegistry() const
        {
            return m_GuiDriverRegistry;
        }

        /// @brief Returns the ImGui layer, or nullptr if the app opted out.
        [[nodiscard]] ImGuiLayer* GetImGuiLayer() const { return m_ImGuiLayer.get(); }

        /// @brief Returns the Gui router consumer that routes UI input into attached documents.
        ///
        /// Registered second in the router registry (behind ImGui) and always present. It walks the
        /// engine's registered viewports for a hosted, interactive document under the pointer or the
        /// focused seat; a document is display-only until the game makes it interactive
        /// (Gui::Document::SetInteractive) while holding its seat.
        [[nodiscard]] Gui::GuiConsumer& GetGuiConsumer() const { return *m_GuiConsumer; }

        /// @brief Registers a viewport into the engine drive-list rendered each frame.
        ///
        /// Stores a non-owning pointer in registration order (which is render order — a producer
        /// viewport registered before its consumer renders first); the caller keeps the owning
        /// Unique from Viewport::Create. The engine hands the viewport a back-reference, so
        /// dropping that Unique self-unregisters it (~Viewport erases its own pointer). Must not
        /// be called from inside the per-frame drive loop. Double-registering a viewport is a
        /// fatal assert.
        /// @param viewport  The viewport to drive; its lifetime stays with the caller.
        void RegisterViewport(Renderer::Viewport& viewport);

        /// @brief Registers a scene capture into the engine drive-list rendered each frame.
        ///
        /// Captures render ahead of every viewport, so a material sampling a capture's output
        /// reads this frame's result — the capture-side analogue of the viewport
        /// registration-order RTT contract. The same ownership model as RegisterViewport: the
        /// caller keeps the owning Unique from SceneCapture::Create, and dropping it
        /// self-unregisters. Double-registering a capture is a fatal assert.
        /// @param capture  The capture to drive; its lifetime stays with the caller.
        void RegisterCapture(Renderer::SceneCapture& capture);

        /// @brief Returns the engine-owned managed viewport set.
        ///
        /// The managed-viewport policy collaborator: GetManagedViewports().Get(0) reaches the primary
        /// (null when ApplicationInfo::ManagedViewport / ManagedViewports is unset), GetCount() the
        /// set size. A game pushes its scene through a returned viewport's SetViewState each frame, or
        /// names a World/Viewer for the engine to resolve. Empty for the editor.
        /// @return The managed viewport set.
        [[nodiscard]] ManagedViewportSet& GetManagedViewports() { return *m_ManagedViewports; }

        /// @brief Returns the engine-owned managed viewport set (const).
        /// @return The managed viewport set.
        [[nodiscard]] const ManagedViewportSet& GetManagedViewports() const
        {
            return *m_ManagedViewports;
        }

        /// @brief Rebuilds the engine-managed viewport set at a safe point.
        ///
        /// Forwards to ManagedViewportSet::Reconfigure: records the requested set and applies it at the
        /// top of the next frame, before any system iteration — never mid-iteration, mirroring the
        /// SetRegion resize debounce. The apply drops removed viewports (RAII self-unregister),
        /// constructs added ones, registers them, and resolves each Layout to pixels; index 0 remains
        /// the primary. Split-screen is a two-element reconfigure. Requires a managed viewport to have
        /// been configured at startup.
        /// @param viewports  The new managed set; each info's Layout, World, Viewer, and render knobs apply.
        void ReconfigureManagedViewports(std::span<const ManagedViewportInfo> viewports);

        /// @brief Re-points a managed viewport at a different world at runtime, applied at the top of frame.
        ///
        /// Forwards to ManagedViewportSet::RebindWorld: records the new world binding and applies it at
        /// the same safe point (top of the next frame) ReconfigureManagedViewports uses, so no rebind
        /// lands mid-drive. The viewport keeps its render target and its bound Viewer; only the world it
        /// presents changes, and its next per-frame camera pull resolves the new world (the old world is
        /// untouched — closing it is a separate WorldRunner::CloseWorld). The presentation-side complement
        /// of opening a world at runtime: open a world, then show it. A no-op for an out-of-range index.
        /// @param index  The managed viewport index (0 the primary).
        /// @param world  The world the viewport presents next.
        void RebindManagedViewport(usize index, WorldInstanceId world);

        /// @brief Re-points a managed viewport at a world once that world is ready, at the top of frame.
        ///
        /// Forwards to ManagedViewportSet::RebindWorldWhenReady: the viewport keeps presenting its
        /// current world until the destination resolves, installs its scene, starts its simulation,
        /// reports its residency batch resident, and ticks at least once, then swaps in one frame (the
        /// departed world's overlays detach and the seat re-resolves atomically) — the front-door / world
        /// jump path, with no empty-world frame and no consumer polling loop. Superseded by a later
        /// rebind of the same index, dropped if the destination closes first, and abandoned (surfaced
        /// through GetAbandonedPresentWorld) if the destination never readies. A no-op for an
        /// out-of-range index.
        /// @param index  The managed viewport index (0 the primary).
        /// @param world  The world to present once it is ready.
        void RebindManagedViewportWhenReady(usize index, WorldInstanceId world);

        /// @brief Returns the world a managed viewport currently presents (its applied binding).
        ///
        /// Forwards to ManagedViewportSet::GetViewportWorld. An in-flight rebind is not reflected until
        /// it applies (read GetPendingManagedViewportWorld for that); an out-of-range index returns the
        /// invalid handle.
        /// @param index  The managed viewport index (0 the primary).
        /// @return The presented world's handle, or an invalid handle when index is out of range.
        [[nodiscard]] WorldInstanceId GetManagedViewportWorld(usize index) const;

        /// @brief Returns the destination of a viewport's in-flight rebind, or nullopt when none pends.
        ///
        /// Forwards to ManagedViewportSet::GetPendingViewportWorld. A pending destination (a deferred or
        /// present-on-ready rebind) counts as presented for lifetime purposes, so it is not reaped in its
        /// own rebind gap.
        /// @param index  The managed viewport index (0 the primary).
        /// @return The pending destination world, or nullopt when no rebind is in flight for the index.
        [[nodiscard]] optional<WorldInstanceId> GetPendingManagedViewportWorld(usize index) const;

        /// @brief Returns the destination a present-on-ready rebind abandoned on timeout, else invalid.
        ///
        /// Forwards to ManagedViewportSet::GetAbandonedPresentWorld: the failure surface of
        /// RebindManagedViewportWhenReady, so a caller can react to a destination that never readied
        /// rather than presenting the old world forever.
        /// @param index  The managed viewport index (0 the primary).
        /// @return The abandoned destination world, or an invalid handle when none was abandoned.
        [[nodiscard]] WorldInstanceId GetAbandonedManagedPresentWorld(usize index) const;

        /// @brief Returns the managed primary viewport's debug-draw accumulator, or null when unconfigured.
        ///
        /// The single-viewport convenience for the canonical per-SceneView DebugDraw channel: it
        /// forwards to GetManagedViewports().Get(0)->GetDebugDraw(). Null when no managed viewport is
        /// configured (ApplicationInfo::ManagedViewport unset), in which case a caller owning its
        /// own Viewport reaches the accumulator through that viewport directly. The debug-draw pass
        /// renders only when the viewport's SceneRendererSettings::DebugDraw is enabled.
        /// @return The primary viewport's DebugDraw accumulator, or nullptr.
        [[nodiscard]] Renderer::DebugDraw* GetDebugDraw() const
        {
            const Renderer::Viewport* primary = m_ManagedViewports->Get(0);
            return primary ? &primary->GetDebugDraw() : nullptr;
        }

        /// @brief Returns the world runner driving every open world.
        ///
        /// The sim-domain scheduler: a game opens further worlds by handle at runtime
        /// (GetWorldRunner().OpenWorld(...)), resolves a world's scene by id
        /// (ResolveWorld(id)->GetScene()), and pauses or queries a world by handle. The engine-managed
        /// world (when ApplicationInfo::World is set) is opened here as world #0 at bootstrap.
        /// @return The world runner.
        [[nodiscard]] WorldRunner& GetWorldRunner() { return *m_WorldRunner; }

        /// @brief Returns the handle of the engine-managed world, or an invalid handle when unmanaged.
        ///
        /// Valid only when ApplicationInfo::World is set, after bootstrap opens the managed world and
        /// binds it to the managed viewport. A game resolves its scene through
        /// GetWorldRunner().ResolveWorld(GetManagedWorldId()). Invalid before bootstrap and for a bare
        /// app that owns no managed world.
        /// @return The managed world's handle.
        [[nodiscard]] WorldInstanceId GetManagedWorldId() const { return m_ManagedWorld; }

        /// @brief Returns the local player's account, or the invalid id when the process has none.
        ///
        /// Resolved once at bootstrap through GameNetInfo::Identity (a process-random ephemeral id
        /// when the hook is unset): the account a client presents at the handshake and the account a
        /// standalone or listen host registers into directory presence per its joins — so
        /// single-player and multiplayer key the player identically. Invalid on a headless dedicated
        /// launch (`--dedicated` / `--server --headless`), where the host is nobody, and before the
        /// managed-world bootstrap runs.
        /// @return The local account id.
        [[nodiscard]] Net::AccountId GetLocalAccount() const { return m_LocalAccount; }

        /// @brief Returns the local account's presented profile, or nullptr when it presented none.
        ///
        /// Resolved once at bootstrap through GameNetInfo::PresentProfile. It is what a client puts
        /// in its connect request and what a listen host or standalone app binds for its own
        /// account, so the local player's profile reads back the same in every topology. The engine
        /// never decodes it.
        /// @return The local profile, borrowed for the process activation, or nullptr.
        [[nodiscard]] const Net::Blob* GetLocalProfile() const
        {
            return m_LocalProfile.Bytes.empty() ? nullptr : &m_LocalProfile;
        }

        /// @brief Returns the process's transport-arm role: Client under `--join`, Server otherwise.
        ///
        /// Server for a standalone app, a listen server, and a dedicated server; Client only when the
        /// launcher activated join mode. This names the process's transport capability (which host it
        /// mounts), a separate axis from what authority role each world ticks under — that is per-world,
        /// stamped onto each world's SystemContext from the host-side world→role map, not from this
        /// process-level value.
        /// @return The process's transport-arm NetRole.
        [[nodiscard]] NetRole GetNetRole() const;

        /// @brief Returns the mounted server host, or null when not hosting.
        ///
        /// Non-null only after a `--server` launch bootstraps the managed world. A game reaches it for
        /// its own traffic or to drain the lifecycle events (ServerHost::Events) its pawn-cleanup rule
        /// watches; the join glue itself needs no game code.
        /// @return The server host, or nullptr.
        [[nodiscard]] ServerHost* GetServerHost() const;

        /// @brief Returns the mounted client host, or null when not joined.
        ///
        /// Non-null only after a `--join` launch or a runtime Connect. A game reaches it to inspect its
        /// own seat / possessed pawn, or to Join further worlds by key; the per-frame join drive is the
        /// engine's.
        /// @return The client host, or nullptr.
        [[nodiscard]] ClientHost* GetClientHost() const;

        /// @brief Starts hosting the managed world at runtime, mounting the ServerHost (mirrors `--server`).
        ///
        /// The runtime counterpart to the `--server` launch flag: binds the listening transport and
        /// stands up the ServerHost on the managed world — with the WorldFactory, Authorize, Placement,
        /// and lifetime hooks from ApplicationInfo::Net — against the same zero-config defaults the
        /// launch flag uses. The managed world becomes Server-tier and accepts connections. A game drives
        /// this from a system (a menu's Host button) after boot; a process that never calls it stays
        /// standalone, exactly as today.
        /// @pre A managed world is configured (ApplicationInfo::World is set) and started, and no net
        ///      mode is already active (neither a launch flag nor a prior runtime call).
        /// @return Empty on success, or an error string if the transport could not be opened.
        VoidResult StartHosting();

        /// @brief Connects to a server as a client at runtime, mounting the ClientHost (mirrors `--join`).
        ///
        /// The runtime counterpart to the `--join` launch flag: binds the connecting transport to the
        /// endpoint and stands up the ClientHost, whose join flow loads the joined world into the managed
        /// world's scene. A game drives this from a system (a menu's Join button) after boot.
        /// @param host  The server host to resolve and connect to.
        /// @param port  The server port, or 0 to use ApplicationInfo::Net's configured default.
        /// @pre A managed world is configured and no net mode is already active.
        /// @return Empty on success, or an error string if the connection could not be opened.
        VoidResult Connect(const string& host, u16 port = 0);

        /// @brief Joins a world by opaque key into its own runner world, returning that world's handle.
        ///
        /// The client complement of the server's per-WorldKey worlds: opens a fresh WorldRunner world,
        /// requests the join over the mounted ClientHost, and installs the replicated scene into that
        /// world when the reply lands (never the managed world #0) — so a joined gameplay world and the
        /// managed world (a front-end, or a second joined world) coexist without colliding. The world
        /// opens paused; the join flow starts its simulation once the reply loads it, ticking it
        /// Client-tier. The returned handle is what a consumer rebinds the managed viewport to
        /// (RebindManagedViewport) to present the joined world. Pair it with
        /// GameNetInfo::AutoJoinDefaultWorld = false to suppress the fixed auto-join and drive every join
        /// explicitly.
        /// @param key       The opaque world to join (the server resolves it through its get-or-place policy).
        /// @param standing  Explicit standing choice for the session record; unset records a
        ///                  standing join (a non-presenting join is standing), false opts out.
        /// @pre A client connection is active (a prior Connect or a `--join` launch).
        /// @return The runner world the join installs its replicated scene into.
        [[nodiscard]] WorldInstanceId JoinWorld(const Net::WorldKey& key,
                                                optional<bool> standing = {});

        /// @brief Joins a world *into an existing live world's scene* — the adopt-in-place join.
        ///
        /// Unlike JoinWorld(key), no fresh runner world is opened and no level is loaded: the join binds
        /// to @p adopt's already-standing scene and streams its spawns into it. The echoed content digest
        /// is still validated (the join is refused fail-loud on mismatch, before any stream applies), so
        /// the consumer must guarantee the standing scene's derived content is a valid reconstruction of
        /// @p key. This is the scene-preserving half of a swap: adopt the destination while the current
        /// join stays live, then LeaveWorld the old one once the destination is ready. The derived and
        /// Local-tier entities are untouched by construction.
        /// @param key       The opaque world to join.
        /// @param adopt     The live runner world whose scene the join binds to (installed and started).
        /// @param standing  Explicit standing choice for the session record; unset records the
        ///                  join as the gameplay entry when @p adopt is presented, standing otherwise.
        /// @pre A client connection is active, and @p adopt resolves to a started scene.
        /// @return @p adopt (the shared world the join now streams into).
        WorldInstanceId JoinWorld(const Net::WorldKey& key, WorldInstanceId adopt,
                                  optional<bool> standing = {});

        /// @brief Leaves a joined world, removing exactly that join's replicated footprint from its scene.
        ///
        /// Destroys the join's wire-owned spawned set, releases its adopted anchor bindings (claimants
        /// survive), demotes its predicted set, drops its per-join net state, and notifies the server so
        /// it tears down the seat. The scene is otherwise untouched — a peer join adopting it stays live —
        /// and the runner world is closed only if no other join still presents its scene (so leaving a
        /// fresh-world join reproduces the old close-on-leave teardown). The scene-preserving half of a
        /// swap and a first-class client operation.
        /// @param join  The JoinId to leave; a no-op for an unknown join.
        void LeaveWorld(Net::JoinId join);

        /// @brief Travels to a destination world — the one primitive across standalone, client, and host.
        ///
        /// Resolves by the process's situation: standalone resolves the key through the world directory
        /// (get-or-place, opening a world through the game's factory on a miss) and present-on-ready
        /// rebinds the named viewport onto it, pinning the destination and unpinning the departed world
        /// so the dwell owns its fate; a client sends a travel request the server answers with a directed
        /// travel (join the resolved world, make-before-break leave the old one); a host resolves locally
        /// and moves its presentation. Total — every failure (authorize denial, caps, factory nullopt,
        /// connect loss) returns through VoidResult with the denial reason. The TravelRequest component
        /// (Veng/Scene/Requests.h) lowers onto this.
        /// @param info  The destination key, opaque arrival payload, presenting viewport, and present flag.
        /// @return Empty on success, or an error string describing why the travel could not run.
        VoidResult Travel(const TravelInfo& info);

        /// @brief Ends a standalone standing membership, releasing its local warm-hold on the world.
        ///
        /// The connectionless counterpart of a client disconnect: a non-presenting standing travel
        /// (Travel with Present false) holds its destination warm under a local directory presence for
        /// the local account; this drops that presence and removes the key from the session's standing
        /// list, so the world's keep-warm accounting sees the local member leave and the dwell owns the
        /// bucket's fate once every other presence (remote joins, other standing holds) is also gone. A
        /// no-op for a key the process holds no standing membership on.
        /// @param key  The standing world to leave.
        void LeaveStanding(const Net::WorldKey& key);

        /// @brief Holds a world warm by key under an accountless infrastructure pin, resolving it first.
        ///
        /// Resolves @p key through the world directory (get-or-place, opening a world through the game's
        /// factory on a miss — a requester-less resolve, so a factory keying off the requester sees the
        /// invalid account), then takes an accountless directory pin on the resolved bucket so it is
        /// never idle-reaped while held. The infrastructure counterpart of a standing join: unlike
        /// LeaveStanding's account-scoped standing-join presence (a Travel with Present false, which
        /// records the local account as a member), this pin belongs to no account — MembersOf never
        /// reports it — so it holds a data world resident for the process itself rather than a player.
        /// It composes with the presence refcount: a world with a warm pin and any joins stays warm
        /// until every pin and join is gone. Idempotent per key — a repeated hold on a key already held
        /// takes no second pin. Prefer this to inflating a world's IdleDwell to keep it resident.
        /// @param key  The world to resolve and hold warm (the release handle for ReleaseWorldWarm).
        /// @return Empty on success, or an error string carrying the directory's denial reason.
        [[nodiscard]] VoidResult HoldWorldWarm(const Net::WorldKey& key);

        /// @brief Releases a warm hold taken by HoldWorldWarm, letting the dwell own the world's fate.
        ///
        /// Drops the accountless pin HoldWorldWarm took on @p key, so the world's keep-warm accounting
        /// sees the infrastructure hold leave; once every other presence (joins, other pins) is also
        /// gone the world reaps after its dwell. A no-op for a key the process holds no warm pin on.
        /// @param key  The warm-held world to release.
        void ReleaseWorldWarm(const Net::WorldKey& key);

        /// @brief Restores the local account's session — the standalone continue, on demand.
        ///
        /// The same registry a reconnect reattaches through, with no wire: consults the local
        /// account's record (loading it through the LoadSession hook) and resolves its entries
        /// against the local directory — a standing join warms its world under a local pin, the
        /// gameplay entry present-on-ready rebinds managed viewport 0 and delivers the recorded
        /// pose. A denied gameplay resolve keeps the record and leaves the current world presented,
        /// so the process lands at its front door and the next attempt retries the same record. A
        /// no-op without a record or a local account.
        ///
        /// The bootstrap calls this itself unless GameWorldInfo::RestoreLocalSessionOnBoot is
        /// false; a game that opted out calls it once the store its record lives in is open. Pair a
        /// restore with ReleaseLocalSession before opening a different store, so the next restore
        /// resolves against that store's record rather than the cached one.
        void RestoreLocalSession();

        /// @brief Tears down the restored local session, so a later restore reloads from scratch.
        ///
        /// The inverse of RestoreLocalSession: drops every standing-join local pin the restore took
        /// (the worlds' dwells then own their fate once no other presence remains) and evicts the
        /// local account's cached record from the session registry, saving it first when dirty. The
        /// path a consumer takes when it leaves the store the record was loaded from: a subsequent
        /// RestoreLocalSession then reloads through LoadSession against whatever store is open, and
        /// carries no presence from the released one. The record's standing list is untouched — this
        /// releases the process's hold on the worlds, it does not resign the account's memberships
        /// (LeaveStanding does that). The presented world is left as it is; the game chooses what to
        /// present next. A no-op when nothing is restored.
        void ReleaseLocalSession();

        /// @brief Tears the net mode down, returning the process to standalone (no transport).
        ///
        /// Releases the mounted host (server or client) and clears the per-world roles, so the managed
        /// world returns to a Server-tier standalone world with no transport bound — the path a
        /// return-to-front-end takes. A no-op when no net mode is active. The world scenes are untouched;
        /// closing or re-opening a world is a separate WorldRunner operation.
        void StopNet();

        /// @brief Returns the level a world was bootstrapped from, or an empty handle.
        ///
        /// Valid only for the engine-managed world; a game reads the level's render settings or
        /// game-mode config from it (e.g. to seed its own editable render-settings copy).
        /// @param world  The world whose source level handle is read.
        /// @return The world's level handle.
        [[nodiscard]] const AssetHandle<Level>& GetWorldLevel(WorldInstanceId world) const;

        /// @brief Returns the per-frame view knobs the managed world pushes into its viewport.
        ///
        /// Seeded from the level's render settings at bootstrap; a game edits it in place (the
        /// tone/bloom/environment knobs a render-settings UI mutates) and the engine fills in the
        /// scene/camera/delta each frame before pushing. Serves the engine-managed world.
        /// @param world  The world whose view knobs are read.
        /// @return The mutable managed-world ViewState.
        [[nodiscard]] Renderer::ViewState& GetWorldViewState(WorldInstanceId world);

        /// @brief Sets a world's explicit pause toggle.
        ///
        /// Forwards to WorldRunner::SetWorldPaused. Paused, the engine still pushes the view each frame
        /// (the camera resolves and the scene renders) and still drives the scene's captures, but runs
        /// no simulation tick — the path a fixed-pose capture or a game pause menu takes. Composes with
        /// any held WorldRunner::PauseScope; a no-op for an unminted world.
        /// @param world   The world to pause or resume.
        /// @param paused  True to stop ticking the world, false to clear the explicit toggle.
        void SetWorldPaused(WorldInstanceId world, bool paused);

        /// @brief Returns whether a world is paused (a held scope or the explicit toggle).
        ///
        /// Forwards to WorldRunner::IsWorldPaused; false for an unminted world.
        /// @param world  The world to query.
        [[nodiscard]] bool IsWorldPaused(WorldInstanceId world) const;

        /// @brief Returns the engine-managed world's current fixed simulation tick number.
        ///
        /// Monotonic, advanced by the managed world's own clock. Zero before the first tick runs, while
        /// the world is paused, and for a bare app with no managed world.
        [[nodiscard]] u64 GetSimTick() const;

        /// @brief Returns this frame's interpolation fraction into the managed world's next Sim tick, in [0, 1).
        ///
        /// The residual accumulator the render gather and View systems blend the last two ticks by.
        /// A game driving its own viewport (or a LevelOverlay) pushes this into its ViewState so its
        /// scene interpolates in phase with the managed world.
        [[nodiscard]] f32 GetSimAlpha() const { return m_SimAlpha; }

    protected:
        /// @brief Called once after all engine systems are initialized.
        virtual void OnInitialize() {}

        /// @brief Called once after the managed world is loaded, before its simulation starts.
        ///
        /// Only fires when ApplicationInfo::World is set. The Scene is spawned and the renderer is
        /// seeded from the level by this point, but the simulation has not started — a game seeds
        /// its own editable render-settings copy, captures input focus, or waits on @p pending
        /// before a deterministic capture here. Default is a no-op (the minimal game needs none).
        /// @param world    The managed world's handle, for resolving it back through the runner.
        /// @param scene    The managed world's Scene (its SceneSimulation attached but not started).
        /// @param pending  The world spawn's not-yet-resident assets; wait on it before a capture.
        virtual void OnWorldLoaded(WorldInstanceId world, Scene& scene, ResidencyBatch& pending) {}

        /// @brief Called once per frame before rendering.
        /// @param delta  Time in seconds since the previous frame.
        virtual void OnUpdate(f32 delta) {}

        /// @brief Called once per frame to record draw commands.
        virtual void OnRender() {}

        /// @brief Runs the app's shutdown operations while every engine service is still alive.
        ///
        /// Called once after the main loop exits and the GPU is idle, immediately before the engine's
        /// own session SaveAll — the seam for shutdown work that must run while the app is fully alive
        /// and interleave with the engine's own operations (an exit checkpoint that must precede the
        /// durability save). It is *not* for resource release: an app releases its resources in its own
        /// destructor, which runs while every engine service is still live, so most apps need no
        /// override. Default is a no-op.
        virtual void OnShutdown() {}

        /// @brief Called in client mode when the own seat's possessed pawn changes (or clears).
        ///
        /// Only fires under `--join`, from the join drive, when the replicated own seat's Possesses
        /// resolves to a newly-bound pawn (Entity::Null when it possesses none) — the point a game
        /// points its Local-tier camera/viewer at that pawn. The camera rig stays untouched
        /// client-local View machinery; this only names its target. Default is a no-op.
        /// @param world  The client scene the ClientHost loaded.
        /// @param pawn   The pawn the own seat now possesses, or Entity::Null.
        virtual void OnClientPossession(Scene& world, Entity pawn) {}

        /// @brief Signals the run loop to exit after the current frame, leaving the exit status.
        ///
        /// The only way to stop a headless app; also works for windowed apps. The status Run
        /// returns is unchanged, so an ordinary quit reports whatever status is already set —
        /// 0 unless RequestExit(i32) named a failure earlier.
        void RequestExit() { m_ShouldExit = true; }

        /// @brief Signals the run loop to exit after the current frame with the given exit status.
        ///
        /// The status becomes Run's return value and, under the launcher, the process exit status;
        /// a non-zero value marks the run as failed. Called from OnInitialize this is the
        /// fatal-startup-failure path: the engine skips the world bootstrap and never enters the
        /// run loop, so no further initialization proceeds. Teardown is unaffected either way —
        /// OnShutdown, the session save, and every destructor still run, which is what this offers
        /// over terminating the process outright. A later call replaces the status.
        /// @param status  The status Run returns; 0 means success.
        void RequestExit(i32 status)
        {
            m_ExitStatus = status;
            m_ShouldExit = true;
        }

        /// @brief Returns the exit status Run will report; 0 until RequestExit(i32) sets one.
        [[nodiscard]] i32 GetExitStatus() const { return m_ExitStatus; }

    private:
        /// @brief The pimpl'd network state (hosts + input buffers); defined in Application.cpp.
        struct NetState;

        void Initialize();
        void Frame();

        /// @brief Mounts the world pack, loads its startup level, and starts the running world.
        ///
        /// Called at the end of Initialize when ApplicationInfo::World is set: mounts the pack
        /// beside the executable, reads its cooked startup level, seeds the managed viewport from
        /// the level's render settings, spawns the world (LoadInto), fires OnWorldLoaded, then
        /// starts the scene's simulation. In client mode the startup-level load is deferred to the
        /// join flow (the level comes from the accept), so this only mounts the packs and connects.
        /// A missing pack or startup level is a fatal assert.
        void BootstrapWorld();

        /// @brief Seeds the managed viewport + view knobs from a started world scene's render settings.
        ///
        /// The shared tail of bringing a world online: applies the scene's LevelRenderSettings onto the
        /// primary viewport's topology and the per-frame view. Used by the server/standalone bootstrap
        /// and by the client when its join-loaded scene starts.
        /// @param world  The world scene to seed the viewport from.
        void SeedViewportFromWorld(Scene& world);

        /// @brief Opens the ServerHost on the started managed world (`--server` and runtime StartHosting).
        ///
        /// Constructs the NetState Server arm from ApplicationInfo::Net (or the zero-config defaults):
        /// listens on the configured port, accepts up to MaxConnections, replicates at the snapshot
        /// interval, and threads the game's hosting hooks (WorldFactory, Authorize, Placement,
        /// MaxPlayersPerInstance, the lifetime knobs). Called from the bootstrap tail after m_World starts
        /// and from StartHosting at runtime.
        /// @param levelId  The managed world's level id folded into its join reply for the client to load.
        /// @return Empty on success, or an error string if the transport could not be opened.
        VoidResult StartServer(AssetId levelId);

        /// @brief Connects the Net::Client and mounts the ClientHost (`--join` and runtime Connect).
        ///
        /// Constructs the NetState Client arm from the endpoint + ApplicationInfo::Net: opens the
        /// connection and installs the join hooks (LoadClientLevel, prefab resolve, OnClientPossession).
        /// The world scene is not loaded here — it arrives through the join flow (LoadClientLevel).
        /// @param host  The server host to resolve and connect to.
        /// @param port  The resolved server port (the caller applies the GameNetInfo default for 0).
        /// @return Empty on success, or an error string if the connection could not be opened.
        VoidResult ConnectClient(const string& host, u16 port);

        /// @brief Constructs the world directory from ApplicationInfo::Net, sharing the runner for teardown.
        ///
        /// Called at bootstrap whenever ApplicationInfo::World is set (transport or not): builds the
        /// role-neutral directory from the Net hooks and caps (or their zero-config defaults), registers
        /// the managed world as a never-reaped bucket, and hands it the runner so a reap tears the closed
        /// world down after the CloseWorld hook. The ServerHost borrows it when hosting is stood up.
        void BuildWorldDirectory();

        /// @brief Reconciles the directory's presentation pins with the managed viewports' bindings.
        ///
        /// The one-directional presentation→lifetime bridge: for each managed viewport, the world it
        /// presents (its applied binding) and any pending rebind destination count as pinned, so a
        /// presented world — pending destination included, so never reaped in its own rebind gap — is
        /// held warm, and a departed world is unpinned so the dwell owns its fate. Run once per frame at
        /// the rebind apply point. The directory never reaches into presentation; Application translates
        /// bindings into pins here.
        void SyncPresentationPins();

        /// @brief Drives the directory's idle reap when no host owns it (the standalone path).
        ///
        /// Standalone, Application reaps the directory each frame (the directory runs the CloseWorld hook
        /// then the runner teardown); while hosting, the ServerHost's Pump owns the reap of the shared
        /// directory, so this is a no-op there to avoid double-reaping.
        void ReapDirectory();

        /// @brief Resolves a TravelInfo standalone through the directory: get-or-place, then present.
        ///
        /// The no-transport arm of Travel: resolves the key through the directory (opening a world
        /// through the game's factory on a miss), and — when presenting — present-on-ready rebinds the
        /// named viewport onto the destination, so SyncPresentationPins pins it and unpins the departed
        /// one at the rebind apply point.
        /// @param info  The travel destination.
        /// @return Empty on success, or the directory's denial reason.
        VoidResult TravelStandalone(const TravelInfo& info);

        /// @brief Takes a local directory presence for a non-presenting standing membership on a world.
        ///
        /// A standing travel and the standalone continue both warm their worlds this way: with no
        /// connection to report a join, the local account's standing membership is a directory pin, so
        /// the world's presence refcount counts the local member beside remote joins and it survives
        /// until every presence leaves. Idempotent per key — a repeated standing hold on a key already
        /// held takes no second presence — and released by LeaveStanding.
        /// @param key    The standing world's key (the release handle).
        /// @param world  The bucket the key resolved to.
        void AcquireLocalStanding(const Net::WorldKey& key, WorldInstanceId world);

        /// @brief Drains the builtin request components across every open world at the frame-safe point.
        ///
        /// Builds the request dispatch from this Application's own operations (StopNet, StartHosting,
        /// Connect, TravelInWorld, RequestExit) and drains each open world's request components in the
        /// fixed type order, applying the handled / pending / failed consumption semantics. Called once
        /// per frame from Frame, right after the deferred managed-viewport reconfigure.
        void DrainRequestComponents();

        /// @brief Loads the accepted level into a joined world's scene, server-authoritative entities skipped.
        ///
        /// The ClientHost's LoadLevel hook: LoadSync the level, LoadInto a scene with
        /// SkipServerAuthoritative (the authored server entities arrive from the stream), install it as
        /// the join's runner-world scene, and retain the residency batch for the deferred OnWorldLoaded.
        /// The install target is the world queued for the in-flight join (the auto-join and each
        /// JoinWorld push one, resolved FIFO in reply order); with no queued target it falls back to the
        /// managed world. The runner owns the installed scene; the host borrows it.
        /// @param id  The level AssetId the accept named.
        /// @return The runner-owned scene the level loaded into.
        [[nodiscard]] Scene* LoadClientLevel(AssetId id);

        /// @brief Resolves the runner world an in-flight join's reply installs into.
        ///
        /// Pops the world queued for the join (FIFO in reply order); with no queued target — a
        /// server-directed travel the ClientHost issued itself — opens a fresh Client-tier runner
        /// world with its own input send window, so a reply never clobbers the managed world or
        /// another join's scene.
        /// @return The runner world the reply's scene installs into.
        [[nodiscard]] WorldInstanceId NextJoinTargetWorld();

        /// @brief Installs an empty scene for a level-less joined world (a stream-populated data world).
        ///
        /// The ClientHost's OpenEmptyWorld hook: for a join reply naming no level, create an empty
        /// scene over the type registry, attach a simulation running no systems (so the world ticks
        /// and starts through the ordinary joined-world path), and install it into the world queued
        /// for the in-flight join — the same target resolution as LoadClientLevel, without a level
        /// load. The runner owns the installed scene; the host borrows it.
        /// @return The runner-owned empty scene the join binds to.
        [[nodiscard]] Scene* OpenEmptyClientWorld();

        /// @brief Seeds the viewport (managed world only), fires OnWorldLoaded, and starts a joined world's scene.
        ///
        /// The client-mode counterpart of the server/standalone bootstrap tail: seeds the managed
        /// viewport when @p world is the managed world, fires OnWorldLoaded (with the retained per-world
        /// residency batch), and starts the simulation under @p world's role. Runs once per joined world,
        /// when the ClientHost's join flow has loaded its scene.
        /// @param world  The runner world the join loaded.
        /// @param scene  The runner-owned scene the join loaded into @p world.
        void StartWorldScene(WorldInstanceId world, Scene& scene);

        /// @brief Rebinds a managed viewport onto a presenting join's freshly installed world.
        ///
        /// The client arm of the present-on-ready front door: when a presenting join (its request or
        /// directed travel carried the Present flag) installs into a fresh runner world, the managed
        /// viewport its travel targeted (viewport 0 for an unprompted directed travel or a reattach's
        /// gameplay restore) rebinds onto that world once it is ready — the same
        /// RebindWorldWhenReady machinery a standalone travel presents through. A join already
        /// presented (the auto-joined managed world) or non-presenting rebinds nothing.
        /// @param join   The JoinId whose world just installed and started.
        /// @param world  The runner world the join installed into.
        void PresentJoinedWorld(Net::JoinId join, WorldInstanceId world);

        /// @brief Closes a joined client world by JoinId: teardown the runner world and its net state.
        ///
        /// The client-side "leave" of a make-before-break directed travel: once the destination join is
        /// ready the ClientHost drops the departed join, and this closes that join's fresh runner world
        /// (WorldRunner::CloseWorld) and clears its per-world net bookkeeping. A no-op for a join with no
        /// tracked runner world.
        /// @param join  The JoinId whose runner world to close.
        void CloseJoinedWorld(Net::JoinId join);

        /// @brief Resolves the JoinId whose replicated scene is @p world's, or ControlJoinId if none.
        ///
        /// Matches a net-active client world to its ClientHost join by scene identity — a join's loaded
        /// scene is the runner world's scene — so the per-world client drive keys its input send,
        /// prediction record, and clock sync by the right JoinId. ControlJoinId before the world's join
        /// reply has loaded its scene.
        /// @param world  The client world to resolve.
        /// @return The world's JoinId, or Net::ControlJoinId when it is not a loaded joined world.
        [[nodiscard]] Net::JoinId ClientJoinForWorld(WorldInstanceId world) const;

        /// @brief Pumps net for every net-active world once this frame: join/accept, apply the stream, feed input.
        ///
        /// The receive+send half of the net world drive, called once per frame after the sim ticks. It
        /// iterates the host-side world→role map (the net-active worlds — those the process's transport
        /// binds) and pumps each through PumpNetWorld. A world absent from the map (a standalone
        /// Server-tier world with no transport) is skipped. A no-op with no net host.
        void PumpNet();

        /// @brief Pumps net for one net-active world under its role: apply the stream, feed input.
        ///
        /// Server-side it pumps the ServerHost (accept → spawn seats, generate + flush the stream at the
        /// world's sim tick, reap) and ingests each connection's redundant input into its jitter buffer;
        /// client-side it pumps the ClientHost (join, apply spawn/despawn + snapshots, wire the own
        /// seat), starts the world scene once it loads, and sends this frame's stamped local input.
        /// @param world  The net-active world being pumped.
        /// @param role   The authority role @p world ticks under (its host-side map entry).
        void PumpNetWorld(WorldInstanceId world, NetRole role);

        /// @brief Returns the authority role @p world ticks under, from the host-side world→role map.
        ///
        /// The per-world authority axis: a world the process's transport binds carries the role its map
        /// entry names (Server for a hosted world, Client for a joined one); a world absent from the map
        /// — a standalone world with no transport — is Server-tier. Read by the world drive to stamp
        /// each world's per-tick SystemContext and to gate its net input feed.
        /// @param world  The world whose role is queried.
        /// @return The world's NetRole; Server when it is not net-active.
        [[nodiscard]] NetRole RoleForWorld(WorldInstanceId world) const;

        /// @brief Returns whether @p world is net-active — bound by the process's transport this frame.
        ///
        /// True for a world in the host-side world→role map (one the mounted host hosts or has
        /// joined) and, on a hosting process, for any live directory bucket — every bucket is
        /// join-visible there (a standalone travel's world converges with a remote join), so its
        /// change ticks stamp and its wire input feeds like any hosted world's. A standalone
        /// Server-tier world with no transport is not net-active, so the drive neither pumps net
        /// for it nor threads the net input feed through its Sim steps.
        /// @param world  The world to test.
        /// @return True when @p world is bound by a mounted host.
        [[nodiscard]] bool IsWorldNetActive(WorldInstanceId world) const;

        /// @brief Returns @p world's current fixed simulation tick, or 0 for an unresolved id.
        /// @param world  The world whose clock tick is read.
        /// @return The world's monotonic sim tick.
        [[nodiscard]] u64 WorldSimTick(WorldInstanceId world) const;

        /// @brief Feeds each ready connection's input scheduled for a server tick into its seat.
        ///
        /// Server-only: consumes the input each connection's client stamped at @p tick from its jitter
        /// buffer (falling back to the underrun coast when it has not arrived) and writes it into the
        /// connection's seat PlayerInput, so the control system re-derives Intent from the wire input at
        /// the matching tick. Called once per Sim step of the hosted world, ahead of the systems.
        /// @param world  The hosted world whose seats are fed.
        /// @param tick   The server sim tick whose matching client input is consumed.
        void FeedServerSeatInputs(WorldInstanceId world, u64 tick);

        /// @brief Stamps this client tick's resolved local input into the input send window.
        ///
        /// Client-only: records the local input seat's resolved PlayerInput for @p clientTick into the
        /// redundant send buffer (drained once per frame by PumpNet). A no-op with no local input seat
        /// in the scene.
        /// @param world       The joined world whose local seat input is stamped.
        /// @param clientTick  The client sim tick being stamped.
        void StampClientInput(WorldInstanceId world, u64 clientTick);

        /// @brief This frame's pointer routing paired with the one scene it is scoped to.
        ///
        /// Entity handles are scene-local, so a routing carries the scene its Owner seat belongs to;
        /// the per-sim assembly hands the routing only to the sim whose scene this names, every other
        /// sim getting an empty routing.
        struct ScopedPointer
        {
            /// @brief The resolved routing (owner seat + region-local position), empty when unscoped.
            PointerRouting Routing;
            /// @brief The scene the routing applies to, or null when no sim receives it this frame.
            const Scene* Scene = nullptr;
        };

        /// @brief Resolves this frame's pointer routing and the one scene it is scoped to.
        ///
        /// Reads the pointer's window point from the Input snapshot and identifies the viewport that
        /// owns it (ResolvePointerViewport): while captured the cursor seat's viewport, else the
        /// associated viewport under the free cursor. The routing is scoped to that viewport's
        /// presented scene, resolving the owner seat scene-locally so no cross-scene handle leaks;
        /// while captured with no associated viewport it falls back to the primary world. Fed into
        /// each ticked sim's SystemContext so only the owning scene's InputMappingSystem sees the
        /// pointer.
        /// @return The routing and the scene it applies to, or an empty routing scoped to no scene.
        [[nodiscard]] ScopedPointer ComputePointerRouting() const;

        /// @brief Builds a ticked simulation's SystemContext, resolving its primary presenting viewport.
        ///
        /// Fills the always-present services (assets, input, tasks), stamps the world's authority @p role
        /// and the given per-scene @p pointer, then resolves the sim's primary presenting viewport — the
        /// first registered Presented viewport whose retained scene is @p scene — to populate View (its
        /// retained camera + region + UI scale) and Debug (its debug-draw sink). View is nullopt and
        /// Debug null for a view-less sim.
        /// @param scene    The scene being ticked.
        /// @param role     The authority role the world ticks under (from RoleForWorld).
        /// @param pointer  This frame's routing for @p scene (empty when the pointer is elsewhere).
        /// @param tick     The tick number to stamp (the Sim step, or the last completed tick in View).
        /// @param alpha    The interpolation fraction to stamp (0 in Sim, the frame residual in View).
        /// @param firstStepThisFrame  True on the frame's first Sim step (false in View); resets a
        ///                            per-frame accumulator (see SystemContext::FirstStepThisFrame).
        /// @param isReplay  True when this is a reconciliation replay step (see SystemContext::IsReplay).
        /// @return The assembled per-tick context.
        [[nodiscard]] SystemContext BuildSystemContext(const Scene& scene, NetRole role,
                                                       const PointerRouting& pointer, u64 tick,
                                                       f32 alpha, bool firstStepThisFrame,
                                                       bool isReplay = false) const;

        ApplicationInfo m_Info;

        /// @brief Command-line arguments parsed once in Run, before Initialize.
        LaunchArguments m_LaunchArgs;

        /// @brief Borrowed from the host; must outlive this app and every Scene it creates.
        TypeRegistry& m_TypeRegistry;

        /// @brief Borrowed from the host; must outlive this app and every SceneSimulation it drives.
        SystemRegistry& m_SystemRegistry;

        /// @brief Borrowed host-owned GuiDriver catalog, or null; must outlive this app if set.
        GuiDriverRegistry* m_GuiDriverRegistry = nullptr;

        /// @brief The application window; null when Headless.
        ///
        /// First of the ordered lifetime members below: they are declared so reverse-declaration
        /// destruction runs the teardown sequence — each releases while every service it borrows is
        /// still alive. Run() ends at its operations (quiesce, OnShutdown, SaveAll) and does no
        /// explicit release; destruction owns the release.
        Unique<Window> m_Window;

        /// @brief Frame-coherent input; borrows m_Window, so declared after it (destructs first).
        Unique<Input> m_Input;

        Renderer::Context m_RenderContext;

        /// @brief Worker pool; declared before m_AssetManager so it destructs after it — the workers
        ///        stop last, since a live load worker holds Context& and AssetManager state.
        Unique<TaskSystem> m_TaskSystem;

        /// @brief Owns every cached asset; borrows m_RenderContext, so declared after it and destructs
        ///        first, retiring each asset's GPU resources while the context is still live.
        Unique<AssetManager> m_AssetManager;

        /// @brief The ImGui integration; borrows m_RenderContext, so declared after it — its backend,
        ///        descriptor pool, and offscreen target release while the device is still alive.
        Unique<ImGuiLayer> m_ImGuiLayer;

        /// @brief Routes window events to ImGui + Input by focus; borrows the window, input, and ImGui
        ///        layer, so it is declared after all three (destructs before them).
        Unique<InputRouter> m_InputRouter;

        /// @brief Renders the registered viewports and composites them to the swapchain each frame.
        ///
        /// Owns the render-order viewport drive-list, the capture drive-list, and the gather +
        /// composite tail (whose passes hold shader AssetHandles). Borrows m_RenderContext at
        /// construction, so it is declared after m_RenderContext (its tail retires before the context)
        /// and after m_AssetManager (its tail's AssetHandles release while the manager is live).
        /// Declared before m_GuiConsumer and m_ManagedViewports, which borrow its drive-list, so it
        /// destructs after them: they self-unregister from the still-live drive-list, and the placement
        /// cache's retained output Refs keep their outputs alive until ~ViewportCompositor clears them.
        Renderer::ViewportCompositor m_Compositor;

        /// @brief The Gui router consumer, registered second (behind ImGui) so attached documents
        ///        receive UI-owned input. Borrows the router/input/window and the compositor's viewport
        ///        drive-list, so it is declared after them (destructs before them).
        Unique<Gui::GuiConsumer> m_GuiConsumer;

        /// @brief The engine-owned managed-viewport policy; empty when no managed viewport is configured.
        ///
        /// Index 0 is the primary. Constructed in Initialize over the compositor/router, built from
        /// ApplicationInfo, and rebuilt by a deferred reconfigure at the top of a frame. Declared after
        /// the compositor, router, and asset manager it borrows so it destructs first — retiring its
        /// viewports against the still-live Context registry and self-unregistering from the still-live
        /// compositor drive-list.
        Unique<ManagedViewportSet> m_ManagedViewports;

        /// @brief The level the managed world was bootstrapped from; empty when World is unset.
        ///
        /// An AssetHandle, so declared after m_AssetManager: it retires through the deferred path while
        /// the asset manager and context are still live.
        AssetHandle<Level> m_WorldLevel;

        /// @brief The sim-domain scheduler owning and ticking every open world.
        ///
        /// Constructed in Initialize over the borrowed registries, asset manager, and context. Declared
        /// after m_AssetManager so it destructs first — its worlds' component AssetHandles (the sky's
        /// environment/material, the level handle) retire through the deferred path while the asset
        /// manager is still live. The managed world is opened here as world #0 at bootstrap; overlays
        /// adopt their scenes into it.
        Unique<WorldRunner> m_WorldRunner;

        /// @brief The host-tier session registry; built beside the directory, borrowed by a mounted host.
        ///
        /// Keeps each account's standing joins and last gameplay world across connections. Application
        /// records the local player's standalone travels into it and resolves the standalone continue at
        /// bootstrap; the ServerHost consumes it (ServerHostInfo::Sessions) when hosting is stood up. The
        /// SaveSession hook fires from Run's SaveAll operation while the app is fully alive.
        Unique<Net::SessionRegistry> m_Sessions;

        /// @brief The role-neutral world directory; built when World is set, borrowed by a mounted host.
        ///
        /// Owns the get-or-place map, presence refcount, keep-warm dwell, and idle reap in every role.
        /// Holds Runner = m_WorldRunner.get(), so declared after m_WorldRunner (destructs before it).
        /// Application resolves standalone travels and drives presentation pins through it; the ServerHost
        /// consumes it (ServerHostInfo::Directory) when hosting is stood up.
        Unique<WorldDirectory> m_Directory;

        /// @brief The pimpl'd net hosts + input buffers; null unless a net launch mode is active.
        ///
        /// The hosts borrow m_Directory and m_Sessions, and a client host borrows a runner-owned world's
        /// scene (whose components hold AssetHandles) and holds each client world's retained level handle
        /// and spawn residency. Declared last of the ordered members so it destructs first — closing its
        /// connections and releasing those borrows before the directory, sessions, world runner, and
        /// asset manager it depends on.
        Unique<NetState> m_Net;

        /// @brief The engine-managed world's handle (world #0); invalid when World is unset.
        ///
        /// Opened at bootstrap and bound to the managed viewport. The world the net host binds to and
        /// whose camera the managed viewport presents; a bare app leaves it invalid. Inert at teardown,
        /// so its declaration order among the trailing plain-data members is immaterial.
        WorldInstanceId m_ManagedWorld;

        /// @brief The local player's account (GetLocalAccount); invalid on a dedicated host.
        Net::AccountId m_LocalAccount;
        /// @brief The local account's opaque profile (GetLocalProfile); empty when none is presented.
        Net::Blob m_LocalProfile;

        /// @brief The standing memberships the local player holds, each pinned present in the directory.
        ///
        /// A standing join's presence standalone is a local pin (there is no connection to report a
        /// join): the standalone continue and every non-presenting standing travel record their world
        /// here, keyed by the world's key so LeaveStanding can withdraw exactly one. The key is the
        /// release handle; the value is the bucket the pin was taken on.
        unordered_map<Net::WorldKey, WorldInstanceId> m_LocalStandingWorlds;

        /// @brief The worlds HoldWorldWarm holds warm, each under an accountless directory pin.
        ///
        /// Keyed by the world's key so ReleaseWorldWarm withdraws exactly one pin and a repeated hold
        /// on a held key is idempotent. Distinct from m_LocalStandingWorlds: those pins carry the local
        /// account (a standing membership), these carry none (an infrastructure hold). The value is the
        /// bucket the pin was taken on.
        unordered_map<Net::WorldKey, WorldInstanceId> m_WarmPinnedWorlds;

        /// @brief The worlds Application currently pins for presentation, keyed by WorldInstanceId value.
        ///
        /// The pin set SyncPresentationPins reconciles each frame against the managed viewports' bindings,
        /// so a pin is added/removed exactly once as a world enters/leaves presentation.
        std::unordered_set<u64> m_PinnedWorlds;

        /// @brief The per-seat request-driven focus tokens the FocusRequest drain owns.
        ///
        /// One token per seat a system has captured gameplay focus for through a FocusRequest; the
        /// engine holds it across frames on the stampers' behalf (they cannot) and pops it when a
        /// UI FocusRequest releases the seat. A FocusToken is a plain id, so dropping the map is inert;
        /// the router owns the actual focus stack.
        unordered_map<Entity, FocusToken> m_FocusRequestTokens;

        /// @brief Per-frame view knobs pushed into the managed viewport; seeded from the level.
        Renderer::ViewState m_WorldView;

        /// @brief This frame's interpolation fraction (GetSimAlpha), retained for the view pushes.
        f32 m_SimAlpha = 0.0f;

        /// @brief Whether the previous frame latched input (an active sim ran zero Sim ticks).
        ///
        /// The edge latch: BeginFrame skips the pressed/released edge roll on a frame following one
        /// that had a live simulation but ran no tick, so an edge survives to the next tick-running
        /// frame. A frame with no active simulation (the editor, a full pause) does not latch — its
        /// input rolls every frame like an ordinary UI, so this stays false there. Starts false so
        /// the first frame rolls cleanly.
        bool m_PreviousFrameLatchedInput = false;

        bool m_ShouldExit = false;

        /// @brief The status Run returns; 0 until RequestExit(i32) names a failure.
        i32 m_ExitStatus = 0;
    };
}
