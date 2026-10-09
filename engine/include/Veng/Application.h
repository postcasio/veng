#pragma once

#include <Veng/Veng.h>
#include <Veng/Assert.h>
#include <Veng/FrameClock.h>
#include <Veng/LaunchArguments.h>
#include <Veng/Window.h>
#include <Veng/Audio/ScopedAudio.h>
#include <Veng/Haptics/ScopedHaptics.h>
#include <Veng/Input.h>
#include <Veng/InputRouter.h>
#include <Veng/Input/SimInputFrame.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/InputMappingContext.h>
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
#include <Veng/Diagnostics/Profiler.h>
#include <Veng/SystemStats.h>
#include <Veng/Task/TaskSystem.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Render/DisplayCapabilities.h>
#include <Veng/Render/FrameRateLimiter.h>
#include <Veng/Audio/AudioResolve.h>
#include <Veng/Capture/VideoRecorder.h>
#include <Veng/Localization/Localization.h>
#include <Veng/Render/GraphicsResolve.h>
#include <Veng/Render/GraphicsSchema.h>
#include <Veng/Render/GraphicsSettings.h>
#include <Veng/Scene/LocalControl.h>
#include <Veng/Scene/PresentationScope.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SimClock.h>
#include <Veng/Scene/SystemRegistry.h>
#include <Veng/World.h>
#include <Veng/WorldRunner.h>
#include <Veng/Scene/Requests.h>
#include <Veng/ManagedViewports.h>
#include <Veng/WorldDirectory.h>

#include <span>
#include <unordered_map>
#include <unordered_set>
#include <utility>

namespace Veng
{
    class ServerHost;
    class ClientHost;
    class GamepadBackend;
    class GuiDriverRegistry;
    class OverlayWorlds;
    class RoleResolver;
    struct CookedProject;
    namespace Gui
    {
        class GuiTranslator;
    }
    namespace Net
    {
        class Client;
    }
    namespace Audio
    {
        class AudioDevice;
        class AudioEngine;
    }
    namespace Haptics
    {
        class HapticsEngine;
    }
    namespace Text
    {
        class GlyphSource;
        class GlyphAtlas;
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
        /// @brief The most Sim steps a world the engine opens runs in one frame (see
        /// WorldOpenInfo::MaxTicksPerFrame). Must be positive.
        u32 MaxTicksPerFrame = 5;
        /// @brief The wall-clock budget for one frame's Sim steps in a world the engine opens, in
        /// milliseconds; unset runs every step MaxTicksPerFrame allows.
        ///
        /// See WorldOpenInfo::MaxSimMillisecondsPerFrame: an overloaded world then dilates time at a
        /// bounded frame cost.
        optional<f32> MaxSimMillisecondsPerFrame;
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
        ///
        /// Read by a host only. A joining client adopts the interval its server's join reply
        /// carries, so a client's own value never shapes its interpolation.
        u32 SnapshotIntervalTicks = 2;
        /// @brief How many recent input ticks each client input packet carries redundantly (the loss window).
        u32 InputRedundancyTicks = 3;
        /// @brief The longest a hosted world rewinds a client's query to judge it as the client saw it, in seconds.
        ///
        /// Each server-hosted world's pose history (Veng/Net/LagCompensation.h) holds this much of
        /// its LagCompensated bodies' past, and a RewindScope clamps a client's view tick to it. It
        /// is also the whole bound on how far lag compensation favours the querying client: a target
        /// that has just moved behind cover can still be hit for up to this long. Inert for a world
        /// that marks nothing LagCompensated or runs no PoseHistorySystem.
        f64 MaxRewindSeconds = 0.25;
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
        /// scale lever that stops bandwidth growing with world size. Zero disables interest (every entity relevant),
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
        /// @brief Client hook yielding the reconcile tolerances a joined WorldKey uses; unset uses the shared value.
        ///
        /// The tolerances counterpart of ClientWorldQuantization: given a WorldKey the client is
        /// joining, returns the ReconcileTolerances that join reconciles against, so a world whose
        /// linear unit is not the metre — where the shared metre-scale Position tolerance would pass
        /// unbounded drift as "matched" and never reach its snap distance — gets a grid sized to its
        /// own scale. Unset threads the shared Tolerances onto every join. Inert off a client.
        function<Net::ReconcileTolerances(const Net::WorldKey&)> ClientWorldTolerances;
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
        /// @brief Judges a travel before it is carried out; unset allows every travel.
        ///
        /// A travel is a world change somebody *asked* for, and the party that decides it is the one
        /// that owns the state it costs — so the engine puts the question to the game on the
        /// **authoritative** side and nowhere else: a standalone or listen host judges its own
        /// player's travel as it resolves it, and a server judges a connected client's travel as it
        /// receives the request, before it directs anything. A client never judges its own.
        ///
        /// Refusing returns the reason. On the local path it becomes the TravelRequest's `Error`, so
        /// a refused travel reports through the request protocol like any other failure; on the
        /// server path the travel is simply not directed, and telling the requester why is the
        /// game's business, since the reason is the game's. Returning nullopt grants it.
        ///
        /// The travel is judged on exactly what a join is judged on (Net::JoinRequestInfo): who asked,
        /// what world they named, and the payload they named it with.
        function<optional<string>(const Net::JoinRequestInfo&)> AuthorizeTravel;
        /// @brief Encodes an account's current gameplay pose from its seat; unset keeps the last travel's.
        ///
        /// The engine cannot serialize game pose, so the game encodes it: invoked at disconnect and
        /// at the save checkpoint with the account's gameplay world and its seat entity there, and
        /// the result is delivered back on reattach. Returning nullopt keeps the record's last pose
        /// standing (a gone or unpawned seat has nothing newer to say), exactly as leaving the hook
        /// unset does for every capture. See Net::SessionRegistryInfo::CaptureTravelPose.
        function<optional<Net::Blob>(WorldInstanceId, Entity)> CaptureTravelPose;
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

    /// @brief The game's net policy as one overridable object — the stateful mirror of GameNetInfo's hooks.
    ///
    /// GameNetInfo's function hooks serve the zero-config consumer: each is a free-standing closure,
    /// built into ApplicationInfo before the application exists, which forces a game whose answers
    /// live in real services (a store, a world factory, per-account state) to capture a deferred
    /// application slot and null-check it in every hook. This interface is the other posture: the
    /// game implements it as an object constructed *after* the application and its services exist,
    /// holding real references, and registers it with Application::SetNetPolicy before the world
    /// bootstrap. A registered policy supersedes the corresponding GameNetInfo hook fields wholesale;
    /// the numeric knobs and the entity-selection rules (PredictionPolicy, InterestPolicy) remain
    /// GameNetInfo's either way.
    ///
    /// Every method's default reproduces the behavior its unset GameNetInfo counterpart selects, so
    /// a policy overrides only the questions it answers — with one deliberate exception: PlaceJoin's
    /// default is pure first-bucket convergence, the MaxPlayersPerInstance = 0 built-in, and a game
    /// capping instance fill under a policy supplies its own placement rule (the knob drives only
    /// the closure path's built-in). Each method's contract — when it runs, what rides in, what the
    /// return means — is documented on its GameNetInfo counterpart and not restated here.
    class VE_API GameNetPolicy
    {
    public:
        virtual ~GameNetPolicy() = default;

        /// @brief The local player's account identity; defaults to minting a process-random ephemeral id.
        /// @see GameNetInfo::Identity
        [[nodiscard]] virtual Net::AccountId Identity() { return Net::GenerateAccountId(); }

        /// @brief The local account's opaque admission profile; defaults to presenting none.
        /// @see GameNetInfo::PresentProfile
        [[nodiscard]] virtual Net::Blob PresentProfile() { return {}; }

        /// @brief Admits or normalizes a presented account; defaults to accepting it as presented.
        /// @see GameNetInfo::AdmitAccount
        [[nodiscard]] virtual optional<Net::AccountId> AdmitAccount(Net::ConnectionId connection,
                                                                    const Net::AccountId& presented)
        {
            return presented;
        }

        /// @brief Judges a travel before it is carried out; defaults to allowing all.
        /// @see GameNetInfo::AuthorizeTravel
        [[nodiscard]] virtual optional<string> AuthorizeTravel(const Net::JoinRequestInfo& request)
        {
            static_cast<void>(request);
            return std::nullopt;
        }

        /// @brief Authorizes a world-join request; defaults to allowing all.
        /// @see GameNetInfo::Authorize
        [[nodiscard]] virtual bool AuthorizeJoin(const Net::JoinRequestInfo& request)
        {
            return true;
        }

        /// @brief Materializes a server world for a joined WorldKey; defaults to resolving none.
        /// @see GameNetInfo::WorldFactory
        [[nodiscard]] virtual optional<ServerWorldResolution>
        ResolveWorld(const Net::JoinRequestInfo& request, const Net::WorldKey& key,
                     const Net::Blob& payload)
        {
            return std::nullopt;
        }

        /// @brief Selects the live bucket a join lands in; defaults to first-bucket convergence.
        ///
        /// Nullopt opens a fresh bucket through ResolveWorld — so the default converges every
        /// joiner of a key on its first live instance, the MaxPlayersPerInstance = 0 built-in.
        /// @see GameNetInfo::Placement
        [[nodiscard]] virtual optional<WorldInstanceId>
        PlaceJoin(const Net::JoinRequestInfo& request,
                  const std::span<const WorldPlacement> buckets)
        {
            return buckets.empty() ? std::nullopt
                                   : optional<WorldInstanceId>{buckets.front().World};
        }

        /// @brief The capture point before a factory-opened world's teardown; defaults to nothing.
        ///
        /// Unlike GameNetInfo::CloseWorld — which owns the whole close when set — this is a
        /// notification: the engine closes the world through its runner after the policy returns,
        /// so an override captures persistent state and never tears the world down itself.
        virtual void OnWorldClosing(const WorldInstanceId world) {}

        /// @brief The content digest the client expects for a joined WorldKey; defaults to the zero digest.
        /// @see GameNetInfo::ClientWorldDigest
        [[nodiscard]] virtual Net::ContentDigest ClientWorldDigest(const Net::WorldKey& key,
                                                                   const Net::Blob& payload)
        {
            return {};
        }

        /// @brief The spatial dequantization grid a joined WorldKey decodes with; nullopt (the default) uses the shared envelope.
        /// @see GameNetInfo::ClientWorldQuantization
        [[nodiscard]] virtual optional<Net::QuantizationSettings>
        ClientWorldQuantization(const Net::WorldKey& key)
        {
            return std::nullopt;
        }

        /// @brief The reconcile tolerances a joined WorldKey uses; nullopt (the default) uses the shared value.
        /// @see GameNetInfo::ClientWorldTolerances
        [[nodiscard]] virtual optional<Net::ReconcileTolerances>
        ClientWorldTolerances(const Net::WorldKey& key)
        {
            return std::nullopt;
        }

        /// @brief Rewrites an account's session record as its reattach begins; defaults to restoring it as recorded.
        /// @see GameNetInfo::TransformOnReattach
        [[nodiscard]] virtual Net::SessionRecord TransformOnReattach(Net::SessionRecord record)
        {
            return record;
        }

        /// @brief Encodes an account's current gameplay pose; nullopt (the default) keeps the last travel's.
        /// @see GameNetInfo::CaptureTravelPose
        [[nodiscard]] virtual optional<Net::Blob> CaptureTravelPose(const WorldInstanceId world,
                                                                    const Entity seat)
        {
            return std::nullopt;
        }

        /// @brief Loads an account's persisted session blob; nullopt (the default) starts it fresh.
        /// @see GameNetInfo::LoadSession
        [[nodiscard]] virtual optional<vector<std::byte>> LoadSession(const Net::AccountId& account)
        {
            return std::nullopt;
        }

        /// @brief Persists an account's session blob; defaults to keeping records process-lifetime.
        /// @see GameNetInfo::SaveSession
        virtual void SaveSession(const Net::AccountId& account, std::span<const std::byte> bytes) {}
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
        /// @brief Whether queue submits return before the driver has encoded the frame.
        ///
        /// Asynchronous by default, so the main thread starts the next frame while the driver
        /// encodes the last; Synchronous forces the encode back inside the submit for debugging.
        /// A user's MVK_CONFIG_SYNCHRONOUS_QUEUE_SUBMITS environment variable wins over this.
        /// Forwarded to ContextInfo::SubmitMode; Context::GetQueueSubmitMode() reports the result.
        Renderer::QueueSubmitMode SubmitMode = Renderer::QueueSubmitMode::Asynchronous;
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
        /// @brief The game's graphics-quality schema asset; nullopt leaves the quality section empty.
        ///
        /// The engine cannot name a game asset, so a game declares its schema here by id. Application
        /// resolves it before OnInitialize and hands it to the GraphicsSettings store
        /// (GetGraphicsSettings()), which it then loads; unset means no schema — the built-in display
        /// group still works and there are no game-authored quality settings. Non-breaking: an app
        /// naming none gets an empty store.
        optional<AssetId> GraphicsSchema = std::nullopt;

        /// @brief The game's audio mixer bus graph asset; nullopt keeps the roots-only default.
        ///
        /// A game declares its complete mixer topology here by id. Application resolves it at boot
        /// and adopts it into the audio engine (ConfigureBusGraph) before the main loop; unset (or
        /// a graph that fails to load or validate) leaves the engine on its built-in roots-only
        /// default (Master ⊃ {Music, SFX, UI, Ambience}), so an app naming none gets the roots free.
        optional<AssetId> AudioBusGraph = std::nullopt;

        /// @brief The game's audio-settings schema asset; nullopt leaves the audio settings domain absent.
        ///
        /// The engine cannot name a game asset, so a game declares its audio schema here by id.
        /// Application resolves it before OnInitialize, constructs the per-machine audio SettingsStore
        /// over it (config audio.json) and loads persisted choices there, then applies them once
        /// through ApplyAudioSettings (GetAudioSettings()) after OnInitialize, once the authored bus
        /// graph is adopted. Unset means no audio settings domain: no store is
        /// constructed and GetAudioSettings() is null, so an engine/headless consumer that wants none
        /// pays nothing. An instance of the same SettingsSchema asset type the graphics schema uses.
        optional<AssetId> AudioSettingsSchema = std::nullopt;

        /// @brief The game's locale index asset; nullopt leaves an inert null-object localization service.
        ///
        /// The engine cannot name a game asset, so a game declares its LocaleIndex here by id.
        /// Application resolves it before OnInitialize, builds the per-machine language SettingsStore
        /// (config locale.json), reads the chosen language (defaulting to the index's source locale),
        /// and constructs the localization service on it — so a consumer initializes against the
        /// index-backed service and the first frame is localized. Unset leaves Application owning an
        /// inert null-object service that
        /// resolves every key to itself (GetLocalization() is still non-null), so a non-localized or
        /// headless consumer is unchanged.
        optional<AssetId> LocaleIndex = std::nullopt;

        /// @brief Construction parameters for the application's profiler (GetProfiler()).
        ///
        /// Sizes its per-thread buffers and the continuous ring — how many seconds of history a
        /// ring dump carries, and the per-thread byte ceiling on reaching it. The defaults suit a
        /// typical frame; a consumer recording a denser one raises the ceiling. Inert under
        /// VE_PROFILE=OFF.
        Diagnostics::ProfilerConfig Profiler;
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
        ///
        /// Once the loop exits, Run ends with its shutdown operations, in order: a running video
        /// capture is finalized; the GPU and the worker pool are drained; OnShutdown runs; the session
        /// records are saved; then every open world is closed, newest first, running each system's
        /// OnStop; and the worker pool is drained again for anything an OnStop queued. So OnShutdown
        /// and the session save see every world open, every world's systems stop while every engine
        /// service and every application member is still alive, and only then, after Run returns,
        /// are the members destroyed.
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
        /// cursor); pop it, or press an action carrying ActionRole::ReleaseFocus, to return input
        /// to the UI. Always present; headless borrows no window and routes nothing.
        [[nodiscard]] InputRouter& GetInputRouter() const { return *m_InputRouter; }

        /// @brief Returns the haptics engine: the per-pad mixer and the one writer of every pad's motors.
        ///
        /// What every scene's SystemContext::Haptics facade routes into, and what tooling inspects
        /// (its one-shots, its layers, each pad's mix) and sets the master intensity on. It runs
        /// headless and on a dedicated host too, where there is simply no device to write; pad state
        /// is read through GetInput.
        /// @pre Run() has initialized the engine — the haptics engine exists only inside Run().
        [[nodiscard]] Haptics::HapticsEngine& GetHaptics() const;

        /// @brief Returns the haptics facade over the application scope, for code outside every scene.
        ///
        /// A debug panel's test, an editor audition, OnUpdate code: what it plays belongs to the
        /// always-Live application scope, so it is never held or muted. A seat target resolves only to
        /// the implicit seat here, since no scene is named. Never for a scene's systems, which play
        /// through their context.
        /// @pre Run() has initialized the engine.
        [[nodiscard]] Haptics::ScopedHaptics GetApplicationHaptics() const;

        /// @brief Returns the registry of presentation scopes: what each scene's sound and rumble does
        ///        this frame.
        ///
        /// Every scene the world runner holds owns a scope here, renewed by its View phase; once per
        /// frame, after OnUpdate, the presentation step resolves every scope's state and then runs each
        /// device engine's once-per-frame update. Its application scope (GetApplicationScope) is the
        /// owner of what application code plays outside any scene. Exists for the application's whole
        /// life and outlives every scene the runner destroys.
        [[nodiscard]] PresentationScopes& GetPresentationScopes() { return m_PresentationScopes; }

        /// @brief Returns the registry of presentation scopes, read-only.
        [[nodiscard]] const PresentationScopes& GetPresentationScopes() const
        {
            return m_PresentationScopes;
        }

        /// @brief Returns the render context.
        [[nodiscard]] Renderer::Context& GetRenderContext() { return m_RenderContext; }

        /// @brief Returns the CPU profiler the engine drives each frame.
        ///
        /// Owned by the application and installed as the process's active profiler at construction,
        /// so the frame spine, simulation, and GPU bridge all record into it. Aggregation is always
        /// live under VE_PROFILE=ON; buffering is what a capture toggles. A shell of no-ops under
        /// VE_PROFILE=OFF.
        [[nodiscard]] Diagnostics::Profiler& GetProfiler() { return m_Profiler; }

        /// @brief Returns the latest process CPU/memory snapshot, sampled at the frame boundary.
        ///
        /// The engine samples once per frame (Frame()), so CpuPercent reads as a per-frame rate and
        /// the memory figures are current as of this frame. Always available regardless of build
        /// configuration; fields the running platform cannot report stay zero (see SystemStats).
        [[nodiscard]] const SystemStats& GetSystemStats() const { return m_SystemStats.GetLast(); }

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

        /// @brief Returns the shared dynamic glyph atlas every font resolves glyphs into.
        ///
        /// @pre Run() has initialized the engine — the atlas exists only inside Run().
        [[nodiscard]] Text::GlyphAtlas& GetGlyphAtlas()
        {
            VE_ASSERT(m_GlyphAtlas, "GetGlyphAtlas before Run(): the glyph atlas exists only once "
                                    "Run() has initialized the engine");
            return *m_GlyphAtlas;
        }

        /// @brief Returns the mixer-facing audio engine.
        ///
        /// What every scene's SystemContext::Audio facade routes into, and what device
        /// configuration (bus gains, bus DSP, the master reverb, the bus graph) and tooling
        /// (GetVoiceInfos) reach; backed by a null device when there is no hardware. Sound is
        /// started through a facade, which names the scope it belongs to.
        /// @pre Run() has initialized the engine — the audio device exists only inside Run().
        [[nodiscard]] Audio::AudioEngine& GetAudioEngine();

        /// @brief Returns the audio facade over the application scope, for code outside every scene.
        ///
        /// An editor audition, a runtime-generation demo, OnUpdate code: what it starts belongs to the
        /// always-Live application scope, so it is never held or muted, and never replay-gated. Never
        /// for a scene's systems, which start sound through their context.
        /// @pre Run() has initialized the engine.
        [[nodiscard]] Audio::ScopedAudio GetApplicationAudio();

        /// @brief Returns the audio device: the output backend and the real-time mixing path.
        ///
        /// The engine surface beneath GetAudioEngine(), for a caller that needs the device itself —
        /// its driven mode and its block tap — rather than the mixer-facing API.
        /// @pre Run() has initialized the engine — the audio device exists only inside Run().
        [[nodiscard]] Audio::AudioDevice& GetAudioDevice();

        /// @brief Drives the frame clock at a fixed delta from the next frame on.
        ///
        /// Every consumer of the frame delta — the simulation, the views, the audio pump — then
        /// advances by FrameClockInfo::Delta per frame however long the frame actually took, so a
        /// slow frame no longer shortens the world's step. Entering also skips the run-loop frame
        /// cap, holds every managed viewport's render scale at its ceiling (Viewport::HoldRenderScale)
        /// so frame-time pressure cannot soften the image, and puts the audio device in driven mode
        /// (AudioDevice::SetDriven), where the hardware is stopped and each pump mixes exactly the
        /// frame's samples. Time::Now() stays wall time throughout, so session timeouts, directory
        /// reaping and the net pumps keep their real cadence.
        ///
        /// Takes effect at the top of the next Frame, so a request made mid-frame never leaves one
        /// frame mixing two modes. Driving an already-driven clock re-bases it at the new delta.
        /// @param info  The driven-mode descriptor.
        /// @pre info.Delta > 0 — asserted otherwise.
        void DriveFrameClock(FrameClockInfo info);

        /// @brief Returns the frame clock to wall time from the next frame on.
        ///
        /// Reverses DriveFrameClock: the frame cap, the viewports' render-scale control and the
        /// audio hardware all resume. The frame cap needs no re-arming — its deadline reset absorbs
        /// the span it was skipped for — and each viewport applies whatever scale or dynamic-
        /// resolution settings were pushed at it while held. Takes effect at the top of the next
        /// Frame.
        void ReleaseFrameClock();

        /// @brief Returns the video recorder: recording the presented frame to a file.
        ///
        /// Always present, and unavailable (Capture::VideoRecorder::IsAvailable()) on a platform
        /// without an encoder or on a headless run, where Start refuses with a reason. A running
        /// capture is stopped and its file finalized before the engine's services are torn down.
        /// @pre Run() has initialized the engine — the recorder exists only inside Run().
        [[nodiscard]] Capture::VideoRecorder& GetVideoRecorder()
        {
            VE_ASSERT(m_VideoRecorder, "GetVideoRecorder before Run(): the video recorder exists "
                                       "only once Run() has initialized the engine");
            return *m_VideoRecorder;
        }

        /// @brief Returns whether the frame clock is currently driven.
        ///
        /// The applied state, not a pending request: a Drive or Release made this frame reads here
        /// only from the next frame on.
        [[nodiscard]] bool IsFrameClockDriven() const;

        /// @brief Returns the host-owned, process-wide registry of reflected types.
        ///
        /// Borrowed: the host constructs it, pre-registers builtins, and calls
        /// VengModuleRegister before passing it here. Must outlive this Application.
        [[nodiscard]] TypeRegistry& GetTypeRegistry() { return m_TypeRegistry; }

        /// @brief Returns the per-machine graphics-settings store.
        ///
        /// Constructed and loaded before OnInitialize, with the schema named by
        /// ApplicationInfo::GraphicsSchema (or none), the type registry, and the per-user config
        /// path — so a consumer reads its persisted choices while initializing and acts on them
        /// before the first presented frame. That one boot Load() is what WasLoadedFromFile()
        /// reports for the rest of the run, so a consumer tells a first run from a returning one
        /// without loading again. Applying is the consumer's: the engine calls
        /// ApplyGraphicsSettings() only when asked — though every viewport registered on the
        /// compositor resolves its scene's RenderLook through OnResolveGraphics against this store.
        /// @pre Run() has reached the settings boot — every hook from OnInitialize on qualifies, a
        ///      subclass constructor does not.
        [[nodiscard]] GraphicsSettings& GetGraphicsSettings()
        {
            VE_ASSERT(
                m_GraphicsSettings,
                "GetGraphicsSettings from a subclass constructor: the graphics-settings store "
                "is built early in Run(), before OnInitialize");
            return *m_GraphicsSettings;
        }

        /// @brief Applies the current graphics settings to every viewport, at a frame-safe point.
        ///
        /// Closes the loop between the chosen values and the renderer without reloading any world.
        /// The scene-shaped half is per viewport: every viewport registered on the compositor runs
        /// OnResolveGraphics over its own scene's RenderLook when that look changes, and this call
        /// has each of them run it once more at its next render (ViewportCompositor::InvalidateLooks)
        /// — configuring only when a topology field actually moved — so two viewports presenting
        /// two looks each compose the choices with their own. The machine-shaped half is applied
        /// here: the built-in display group, the output calibration (OutputBrightness /
        /// OutputGamma, filled from the built-in display selections and carried on every managed
        /// and overlay push — the single writer of those two knobs), and, for each managed
        /// viewport, the render scale and the dynamic-resolution choice resolved against its
        /// scene's look, with the Global facet taken from the primary's.
        ///
        /// A viewport presenting a scene with no RenderLook resolves nothing: its topology is its
        /// owner's. It no-ops cleanly on an empty managed set (the editor) beyond the display group,
        /// so the menu calls it on Apply and the boot path calls it after loading the store; a later
        /// world, rebind or overlay needs no re-apply, since its viewport resolves its look itself.
        void ApplyGraphicsSettings();

        /// @brief Returns the per-machine audio-settings store, or null when the domain is absent.
        ///
        /// Constructed and loaded before OnInitialize, with the schema named by
        /// ApplicationInfo::AudioSettingsSchema, the type registry, and the per-user config path
        /// (audio.json); null when no schema is named (the domain is absent), and null from a
        /// subclass constructor, which runs before the store is built. The menu reads and writes it;
        /// the engine applies it once at boot, after the authored bus graph is adopted.
        /// @return The audio store, or nullptr when no audio schema was named.
        [[nodiscard]] SettingsStore<SettingsChoices>* GetAudioSettings()
        {
            return m_AudioSettings.get();
        }

        /// @brief Returns the localization service — always non-null.
        ///
        /// A real, index-backed service when ApplicationInfo::LocaleIndex is set and its index loads,
        /// else the inert null-object that resolves every key to itself. Consumers resolve
        /// user-facing text through it; SystemContext::Localization binds to the same service. Both
        /// the chosen language (read from the per-machine locale.json) and the service built on it
        /// are resolved before OnInitialize, so text initialization reads through the index.
        /// @pre Run() has reached the settings boot — every hook from OnInitialize on qualifies, a
        ///      subclass constructor does not.
        /// @return The localization service.
        [[nodiscard]] Localization::Localization& GetLocalization() const
        {
            VE_ASSERT(m_Localization,
                      "GetLocalization from a subclass constructor: the localization service is "
                      "built early in Run(), before OnInitialize");
            return *m_Localization;
        }

        /// @brief Returns the per-machine language-settings store, or null when no locale index is set.
        ///
        /// Constructed and loaded before OnInitialize beside the localization service, and persisted
        /// as locale.json under the per-user config directory; it carries the chosen "language"
        /// option the boot reads to pick the active locale. A language selector writes the chosen
        /// locale id into it and saves, so the choice survives a restart; null when
        /// ApplicationInfo::LocaleIndex names no index or its index failed to load, and null from a
        /// subclass constructor, which runs before the store is built.
        /// @return The language store, or nullptr when no locale index was named.
        [[nodiscard]] SettingsStore<SettingsChoices>* GetLanguageSettings()
        {
            return m_LanguageSettings.get();
        }

        /// @brief Resolves the current audio settings and applies the resulting bus gains to the mixer.
        ///
        /// Closes the loop between the chosen values and the mixer: it pre-fills the resolve output
        /// with one entry per active-graph bus at its graph-default gain, invokes OnResolveAudio once
        /// so the game maps the player's chosen values onto bus gains, then applies each resolved
        /// {bus, gain} through AudioEngine::SetBusGain. A no restart, no reload apply — SetBusGain
        /// takes effect on the next mix, so a settings change is audible immediately (already-playing
        /// voices included). Applying is idempotent and cheap: re-applying every gain each call — even
        /// unchanged ones — costs nothing on the audio thread (the mixer republishes once per frame).
        ///
        /// A no-op when the audio settings domain is absent (no schema named) or there is no audio
        /// device. The boot path calls it after loading the store and adopting the bus graph; the
        /// menu calls it on Apply.
        void ApplyAudioSettings();

        /// @brief Reports what the hardware offers for the built-in Display group, at runtime.
        ///
        /// The connected monitors (each with its supported resolutions and refresh rates, from GLFW),
        /// the present modes the surface supports (from the swapchain query), and the fullscreen modes
        /// the platform offers (a single native toggle on macOS, the three-way set elsewhere). This is
        /// what the built-in Display dropdowns are populated from and what ApplyBuiltinDisplay validates
        /// a selection against. Returns empty monitors on a headless/dedicated run (no window).
        /// @return The live display capabilities.
        [[nodiscard]] DisplayCapabilities GetDisplayCapabilities() const;

        /// @brief Applies the engine's built-in display selections to the live window and swapchain.
        ///
        /// The display half of the settings apply, extending ApplyGraphicsSettings (which calls it).
        /// Validates @p display against GetDisplayCapabilities() first — an absent monitor falls back to
        /// the primary, a mode the hardware cannot drive clamps to the nearest supported, and a
        /// fullscreen mode the platform does not offer drops to its fullscreen choice — so a settings
        /// file naming gone hardware
        /// never lands on a black or off-screen surface. It then diffs against the current display
        /// state and does only the needed work: a resolution / fullscreen / monitor / refresh change
        /// re-applies the window (recreating the swapchain through the existing resize path), a
        /// present-mode change recreates the swapchain, a frame-cap change re-sets the run-loop limiter
        /// (no swapchain work). Render scale and brightness/gamma are not applied here — they route
        /// through the resolve output in ApplyGraphicsSettings. A no-op on a headless run beyond the
        /// frame cap. Safe at a frame-safe point; the window/swapchain recreation lands at the next
        /// BeginFrame.
        /// @param display  The built-in display selections to apply.
        void ApplyBuiltinDisplay(const BuiltinDisplayChoices& display);

        /// @brief Persists a full-screen change the user made outside the settings path.
        ///
        /// The macOS green title-bar button (and any OS full-screen gesture) toggles the window's
        /// native state directly, bypassing ApplyBuiltinDisplay — so the persisted store would go
        /// stale and boot would re-apply the old mode. Called once per frame, this observes the
        /// window's live full-screen mode: on a change that is not the engine's own async
        /// ApplyDisplayMode landing (told apart by the live mode differing from m_ActiveDisplay's), it
        /// writes the mode into the graphics store's display choices, updates m_ActiveDisplay, and
        /// saves. A no-op without a window or a settings store, and inert where GetFullscreenMode only
        /// moves through the engine's own apply.
        void SyncUserFullscreenChange();

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

        /// @brief Sets the input context Gui navigation resolves from, beneath every seat's own.
        ///
        /// The engine binds no key to navigation: it resolves the role-tagged actions
        /// (ActionRole) of this context every frame — beneath each seat's InputContextStack, and
        /// alone for the implicit seat that drives viewports bound to no seat — and drives the
        /// pressing seat's interactive documents. A managed game takes it from its cooked project's
        /// default UI context before OnInitialize, so a call here overrides the project's choice; an
        /// empty handle leaves documents navigable by pointer alone.
        /// @param context  The UI context, or an empty handle for none.
        void SetDefaultUiContext(AssetHandle<InputMappingContext> context);

        /// @brief Returns the input context Gui navigation resolves from (empty when none is set).
        [[nodiscard]] const AssetHandle<InputMappingContext>& GetDefaultUiContext() const
        {
            return m_DefaultUiContext;
        }

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
        /// rebind of the same index, and abandoned (surfaced through GetAbandonedPresentWorld) if the
        /// destination never readies or vanishes mid-wait (idle-reaped or closed out from under the
        /// wait). A no-op for an out-of-range index.
        /// @param index  The managed viewport index (0 the primary).
        /// @param world  The world to present once it is ready.
        void RebindManagedViewportWhenReady(usize index, WorldInstanceId world);

        /// @brief Sets the consumer predicate every present-on-ready rebind must also satisfy.
        ///
        /// Forwards to ManagedViewportSet::SetPresentReadyGate. The engine's own readiness test is
        /// necessary but not always sufficient: a consumer whose destination world runs per-world work
        /// that must finish before the first visible frame — a bake, a stream, a generation pass —
        /// answers here, and the outgoing world stays presented meanwhile. The wait clock keeps
        /// running while the gate refuses, so a gate that never opens abandons through the ordinary
        /// timeout path (GetAbandonedManagedPresentWorld) rather than stranding the viewport. An empty
        /// function (the default) presents on the engine's test alone.
        /// @param gate  The predicate, or an empty function to remove the gate.
        void SetWorldPresentReadyGate(WorldPresentReadyGate gate);

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

        /// @brief Returns the destination a present-on-ready rebind abandoned, else invalid.
        ///
        /// Forwards to ManagedViewportSet::GetAbandonedPresentWorld: the failure surface of
        /// RebindManagedViewportWhenReady reporting every terminal non-completion — a destination
        /// that stayed unready across the bounded retries, or vanished mid-wait — so a caller can
        /// react rather than presenting the old world forever. The retries themselves are the
        /// engine's (see ManagedViewportSet::PresentReadyAttempts); by the time this reports, the
        /// wait has already been given every chance.
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

        /// @brief Returns the application's world directory, or null when it holds none.
        ///
        /// The directory is the application's in every role: a standalone app resolves its travels
        /// through it, a mounted ServerHost borrows it, and a joining client still owns one. Reaching
        /// it is how a game asks the world-lifetime questions the engine already answers — which
        /// instances a key is live under (WorldDirectory::InstancesOf), who is present in it
        /// (MembersOf), whether an instance is still live (Contains) — instead of mirroring that
        /// state against registration and close notifications.
        /// @return The directory, or nullptr before the managed world is started (and always for an
        ///         application configured without one).
        [[nodiscard]] WorldDirectory* GetWorldDirectory() const;

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

        /// @brief Registers the game's net policy, superseding GameNetInfo's hook closures.
        ///
        /// Call from OnInitialize at the latest: the policy is consulted at every later hook
        /// consumption point — the identity resolution and directory/session-registry build at the
        /// world bootstrap, and each host mount — and registering one after the directory exists is
        /// asserted against, since the hooks already threaded would ignore it. The object is
        /// borrowed and must outlive Run. Null detaches nothing: registration is one-way, a
        /// simplification the assert already implies.
        /// @param policy  The game's policy object; borrowed for the application's lifetime.
        void SetNetPolicy(GameNetPolicy* policy);

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
        ///
        /// Returns whether a recorded gameplay world was resolved and its presenting rebind
        /// requested. False is the vacuous restore — no record, no gameplay entry in it, or a
        /// resolve the directory denied — after which nothing will present, so a caller that was
        /// counting on the restore to put a world on screen must fall back to a travel of its own
        /// rather than wait.
        /// @return True when a gameplay world's present-on-ready rebind was requested.
        bool RestoreLocalSession();

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
        /// Valid only for the engine-managed world; a game reads the level's authored data from it
        /// (its game-mode config, the render block its scene's RenderLook was seeded from).
        /// @param world  The world whose source level handle is read.
        /// @return The world's level handle.
        [[nodiscard]] const AssetHandle<Level>& GetWorldLevel(WorldInstanceId world) const;

        /// @brief Sets a world's explicit pause toggle.
        ///
        /// Forwards to WorldRunner::SetWorldPaused. Paused, the engine still pushes the view each frame
        /// (the camera resolves and the scene renders) and still drives the scene's captures, but runs
        /// none of the world's systems, Sim or View — the path a fixed-pose capture or a game pause
        /// menu takes. Composes with any held WorldRunner::PauseScope and request-driven pause
        /// (PauseRequest); a no-op for an unminted world or one with no simulation.
        /// @param world   The world to pause or resume.
        /// @param paused  True to stop ticking the world, false to clear the explicit toggle.
        void SetWorldPaused(WorldInstanceId world, bool paused);

        /// @brief Returns whether a world is paused (a held scope or the explicit toggle).
        ///
        /// Forwards to WorldRunner::IsWorldPaused; false for an unminted world or one with no
        /// simulation.
        /// @param world  The world to query.
        [[nodiscard]] bool IsWorldPaused(WorldInstanceId world) const;

        /// @brief Sets what a world's request components may reach when the engine drains them.
        ///
        /// Every world drains its requests as WorldRequestMode::Full with no OnExit until a policy is
        /// set. A policy replaces any the world held, and is dropped when the world closes. A world
        /// that is one part of an application rather than the game — a tool's play session, an
        /// overlay that ends itself — is the case it exists for (see WorldRequestPolicy).
        /// @param world   The world the policy applies to.
        /// @param policy  The mode and exit handler its requests drain under.
        void SetWorldRequestPolicy(WorldInstanceId world, WorldRequestPolicy policy);

        /// @brief Returns the viewport an open overlay renders into, or null when @p overlay is not one.
        ///
        /// For presentation code — a host reading the overlay's ViewState, or projecting a point
        /// into it. A system reaches the overlay through its opener's LevelOverlayState and the
        /// overlay's own scene, never through its viewport. Valid until the overlay closes.
        /// @param overlay  The overlay's world (LevelOverlayState::World).
        /// @return The overlay's Presented viewport, or nullptr.
        [[nodiscard]] Renderer::Viewport* FindOverlayViewport(WorldInstanceId overlay) const;

        /// @brief Returns the engine-managed world's current fixed simulation tick number.
        ///
        /// Monotonic, advanced by the managed world's own clock. Zero before the first tick runs, while
        /// the world is paused, and for a bare app with no managed world.
        [[nodiscard]] u64 GetSimTick() const;

        /// @brief Returns this frame's interpolation fraction into the managed world's next Sim tick, in [0, 1).
        ///
        /// The residual accumulator the render gather and View systems blend the last two ticks by.
        /// A game driving its own viewport pushes this into its ViewState so its scene interpolates
        /// in phase with the managed world.
        [[nodiscard]] f32 GetSimAlpha() const { return m_SimAlpha; }

    protected:
        /// @brief Called once after all engine systems are initialized.
        virtual void OnInitialize() {}

        /// @brief Called once after the managed world is loaded, before its simulation starts.
        ///
        /// Only fires when ApplicationInfo::World is set. The Scene is spawned (its RenderLook
        /// seeded from the level) by this point, but the simulation has not started — a game
        /// adjusts the scene's look, captures input focus, or waits on @p pending before a
        /// deterministic capture here. It does not fire for an overlay world (OnOverlayLoaded
        /// does). Default is a no-op (the minimal game needs none).
        /// @param world    The managed world's handle, for resolving it back through the runner.
        /// @param scene    The managed world's Scene (its SceneSimulation attached but not started).
        /// @param pending  The world spawn's not-yet-resident assets; wait on it before a capture.
        virtual void OnWorldLoaded(WorldInstanceId world, Scene& scene, ResidencyBatch& pending) {}

        /// @brief Called once per overlay a LevelOverlay opens, after its level loads and its seed lands, before it starts.
        ///
        /// The seam for what a Seed cannot carry — state with no reflected form, such as a shared
        /// immutable resource or a cache — so a system's OnStart in the overlay sees it. The
        /// overlay's world is open but unstarted and not yet presented. OnWorldLoaded does not fire
        /// for an overlay world, so an application's world setup never runs against one. Default
        /// is a no-op.
        /// @param opener   The world whose LevelOverlay requested the overlay.
        /// @param entity   The entity carrying the LevelOverlay.
        /// @param overlay  The overlay's world.
        /// @param scene    The overlay's scene, its simulation attached but not started.
        virtual void OnOverlayLoaded(WorldInstanceId opener, Entity entity, WorldInstanceId overlay,
                                     Scene& scene)
        {
        }

        /// @brief Called when a presenting travel's rebind lands on its destination world.
        ///
        /// Where OnWorldLoaded fires once, when a world is *created*, this fires once per *travel* —
        /// the moment a present-on-ready rebind (Travel) actually flips the viewport onto its
        /// destination, whether that world was freshly opened or an already-live one reused. It
        /// carries the travel's payload so a game can apply arrival state (spawn pose, and the like)
        /// even into a reused world, whose OnWorldLoaded ran a session ago and whose factory did not
        /// re-run. @p reused distinguishes the two: false for a world this travel opened (its
        /// OnWorldLoaded fired this travel too), true for a reused live world (it did not). Fires only
        /// for a local presenting travel; a data-world or non-presenting resolve does not present and
        /// so does not arrive. Default is a no-op.
        /// @param world    The destination world's handle.
        /// @param scene    The destination world's Scene, live and started.
        /// @param payload  The travel's opaque arrival payload; may be empty.
        /// @param reused   True when the destination was an already-live world, false when this travel
        ///                 opened it.
        virtual void OnWorldArrival(WorldInstanceId world, Scene& scene, const Net::Blob& payload,
                                    bool reused)
        {
        }

        /// @brief Called once when a managed viewport starts presenting a world, after its seat is adopted.
        ///
        /// Fires per completed rebind — deferred or present-on-ready — at the frame-safe point the
        /// rebind applied on, after the viewport's seat association, the cursor seat and the
        /// unbound-seat resolution have all settled. It is the moment the polling alternative
        /// reconstructs ("has the viewport reached this world, and is its seat bound yet"), which is
        /// why the seat rather than only the world is carried: a consumer that gives a presented seat
        /// its input posture — releasing gameplay focus for a cursor-driven world, capturing it for a
        /// flight one — stamps its one FocusRequest here. Focus policy stays the consumer's; the
        /// engine writes none. Default is a no-op.
        /// @param index  The managed viewport index that completed its rebind (0 the primary).
        /// @param world  The world the viewport now presents.
        /// @param seat   The seat adopted for the viewport, or Entity::Null when none resolved.
        virtual void OnWorldPresented(usize index, WorldInstanceId world, Entity seat) {}

        /// @brief Called once when a present-on-ready rebind gives up on its destination.
        ///
        /// The delivered form of GetAbandonedManagedPresentWorld: a present-on-ready request that
        /// stayed unready across its retries, or whose destination vanished mid-wait, is abandoned and
        /// the viewport keeps its current world. Fires on the frame it happens, so a consumer aborts a
        /// transition — retiring a loading screen, returning a front end to idle — instead of comparing
        /// the record against a remembered value every frame. The record itself stands until a later
        /// rebind of the index supersedes it, for a reader that arrives late. Default is a no-op.
        /// @param index        The managed viewport index whose request was abandoned (0 the primary).
        /// @param destination  The world the request never presented.
        virtual void OnWorldPresentAbandoned(usize index, WorldInstanceId destination) {}

        /// @brief Called once when the managed viewports stop presenting a world that stays open.
        ///
        /// Fires on the frame the last managed viewport presenting @p world stops presenting it —
        /// typically because the viewport moved to another world — while the world is still alive,
        /// still holds its presentation pin in the world directory (for a world the directory
        /// tracks), and has not ticked since the last frame that presented it, so a consumer
        /// tidies what its local player left there (a pawn, a claim) before anything else sees it. It
        /// runs after OnWorldPresented, so in the frame of a switch the destination's presentation is
        /// reported before the departure from the source. It does not fire for a world that closes
        /// while presented (OnWorldClosing covers that), for a pending destination that was abandoned
        /// before it presented (OnWorldPresentAbandoned), or at shutdown. The world is unpinned after
        /// the hook returns. Default is a no-op.
        /// @param world  The world the managed viewports stopped presenting, still live.
        virtual void OnWorldDeparted(World& world) {}

        /// @brief Composes the player's chosen graphics quality with a scene's authored look.
        ///
        /// The resolve seam ApplyGraphicsSettings invokes once per apply: given the user's chosen
        /// values and the authoring context (@p input), it produces the concrete renderer state the
        /// engine applies (@p output). The engine does not fix how the two axes combine — a game maps
        /// its own quality options onto the renderer surfaces here, reaching global knobs and per-object
        /// features the engine cannot tier generically.
        ///
        /// It is also the look resolver of every viewport registered on the compositor (managed,
        /// overlay or the consumer's own): a viewport runs it over the RenderLook of the scene it
        /// presents whenever that look changes and once after each ApplyGraphicsSettings, with
        /// input.AuthoredLook that look, configures output.Settings and writes the look-owned fields
        /// of output.View over every view pushed to it — so the user's choices are never reverted
        /// by a presentation change or a look edit; the dynamic-resolution choice and the Global
        /// facet are applied by ApplyGraphicsSettings alone. A resolver is therefore called many
        /// times per apply and must be a pure function of its input.
        ///
        /// @p output arrives pre-filled with the authored baseline (the authored look mapped onto the
        /// two renderer surfaces plus the viewport's current dynamic-resolution choice), so the default
        /// implementation does nothing and returns that baseline unchanged — an app that declares no
        /// schema is byte-identical to one that never resolved. A resolver must be total on the input,
        /// including the default-constructed authored look the no-world case supplies. It must not set
        /// output.View.OutputBrightness/OutputGamma — those are engine-owned display calibration the
        /// apply path writes from the built-in display selections, and a viewport carries none of
        /// output.View's fields a look does not own (CopyLookKnobs).
        /// @param input   The chosen values and the authoring context to compose with.
        /// @param output  The renderer state to apply, pre-filled with the authored baseline.
        virtual void OnResolveGraphics(const GraphicsResolveInput& input,
                                       GraphicsResolveOutput& output)
        {
            static_cast<void>(input);
            static_cast<void>(output);
        }

        /// @brief Maps the player's chosen audio settings onto the mixer's bus gains.
        ///
        /// The resolve seam ApplyAudioSettings invokes once per apply: given the chosen values and
        /// the active bus graph (@p input), it produces the bus gains the engine applies (@p output).
        /// The engine does not fix what a choice means — a game maps its own audio options (a volume
        /// slider through a perceptual taper, say) onto bus gains here.
        ///
        /// @p output arrives pre-filled with one entry per active-graph bus at its graph-default
        /// gain, so the default implementation does nothing and leaves every bus at its authored
        /// default — an app that declares no audio schema never reaches this. A resolver overrides
        /// the entries it cares about through AudioResolveOutput::SetGain and leaves the rest.
        /// @param input   The chosen values and the active bus graph to map onto gains.
        /// @param output  The bus gains to apply, pre-filled with the graph's default gains.
        virtual void OnResolveAudio(const AudioResolveInput& input, AudioResolveOutput& output)
        {
            static_cast<void>(input);
            static_cast<void>(output);
        }

        /// @brief Resolves an audio store against a mixer and applies the resulting bus gains.
        ///
        /// The reusable core the public ApplyAudioSettings() drives with the app's own store and
        /// engine: it pre-fills @p output-equivalent graph defaults from @p engine, invokes
        /// OnResolveAudio, and applies each resolved gain through @p engine's SetBusGain. Separated
        /// so the resolve/apply path is exercisable against a specific engine and store.
        /// @param engine  The mixer whose bus gains are set.
        /// @param store   The audio store supplying the resolve input's chosen values.
        void ApplyAudioSettings(Audio::AudioEngine& engine,
                                const SettingsStore<SettingsChoices>& store);

        /// @brief Called once per frame before rendering.
        /// @param delta  Time in seconds since the previous frame.
        virtual void OnUpdate(f32 delta) {}

        /// @brief Called once per frame to record draw commands.
        ///
        /// Builds the frame's immediate-mode UI when one is open — check
        /// `GetImGuiLayer()->IsFrameOpen()`, which is false on a frame IsImGuiFrameWanted declined.
        virtual void OnRender() {}

        /// @brief Whether this frame runs an immediate-mode UI (ImGui) frame.
        ///
        /// Consulted once per frame when the app has an ImGuiLayer, after the frame's input is
        /// routed and before the worlds tick, so a change made during a frame (a panel toggled in
        /// OnUpdate) takes effect from the next. Returning false skips the ImGui frame outright —
        /// no NewFrame, no Render, no overlay pass, nothing composited over the scene — so an app
        /// whose debug UI is closed pays nothing for ImGui; OnRender must then make no ImGui call
        /// that frame. Default is true: every frame runs one.
        /// @return True when OnRender will build immediate-mode UI this frame.
        [[nodiscard]] virtual bool IsImGuiFrameWanted() const { return true; }

        /// @brief Runs the app's shutdown operations while every engine service is still alive.
        ///
        /// Called once after the main loop exits and the GPU is idle, immediately before the engine's
        /// own session SaveAll — the seam for shutdown work that must run while the app is fully alive
        /// and interleave with the engine's own operations (an exit checkpoint that must precede the
        /// durability save). It is *not* for resource release: an app releases its resources in its own
        /// destructor, which runs while every engine service is still live, so most apps need no
        /// override. Default is a no-op.
        ///
        /// Every world is still open here, and stays open through the session save; each world's
        /// systems stop (OnStop) after both. So a service this override stops is already gone when
        /// those OnStops run — an OnStop needing it releases through it itself, or tolerates its
        /// absence. An OnStop wanting its effect made durable flushes it itself, since the save has
        /// already run.
        virtual void OnShutdown() {}

        /// @brief Called when the pawn this machine's own seat controls changes (or clears).
        ///
        /// The event form of the LocalControl marker (Veng/Scene/LocalControl.h): it fires in **every**
        /// mode, at the marker's own derivation points, so a listen host — where one peer both hosts
        /// and presents — is notified exactly as a joined client is. A game points its Local-tier
        /// camera/viewer at the named pawn here; the camera rig stays untouched client-local View
        /// machinery and this only names its target. Entity::Null means the seat controls nothing.
        ///
        /// It fires once per transition. On a client the join drive reports the replicated own seat's
        /// possession as the stream binds it — which is what a game turns into its presenting seat's
        /// Possesses — and the frame's marker sweep, which reads that presenting seat, does not repeat
        /// it. The marker is the single source of truth for the state; this is its edge. Default is a
        /// no-op.
        /// @param world  The scene the possession changed in.
        /// @param pawn   The pawn the own seat now controls, or Entity::Null.
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

        /// @brief Applies a requested frame-clock mode change at the top of a frame.
        ///
        /// Entering and leaving both move the frame clock, the managed viewports' render-scale hold
        /// and the audio device's driven mode together, before the frame's delta is sampled, so no
        /// frame runs with the three out of step.
        void ApplyPendingFrameClock();

        /// @brief Samples the per-frame profiler counters (task pool, net telemetry) once per frame.
        void SampleFrameCounters();

        /// @brief Samples the frame's draw-call count and the renderer's cull-funnel and point-field
        ///        counters at the render-block end.
        void SampleRenderCounters();

        /// @brief Lifts the last completed frame's GPU pass timings onto the virtual GPU track.
        ///
        /// Run once per frame after Context::EndFrame(). The timings read back are N frames old, so
        /// each pass is stamped with the frame index that executed it — tracked per frame-in-flight
        /// slot — not the current one. Reaches the timings only through the public Context accessors.
        void BridgeGpuTimings();

        /// @brief Reads the cooked project beside the executable and mounts its packs.
        ///
        /// Called before OnInitialize when ApplicationInfo::World is set, so a game can load a cooked
        /// asset (a startup palette, a config table, a boot UI atlas) during initialization — before
        /// the world bootstrap runs. The parsed project is returned for BootstrapWorld to reuse rather
        /// than re-read. A missing project file or pack is a fatal assert.
        /// @return The parsed cooked project, its packs mounted.
        [[nodiscard]] CookedProject MountProjectPacks();

        /// @brief Loads the world's startup level and starts the running world.
        ///
        /// Called at the end of Initialize when ApplicationInfo::World is set, after MountProjectPacks
        /// has mounted the project's packs: reads the cooked startup level, seeds the managed viewport
        /// from the level's render settings, spawns the world (LoadInto), fires OnWorldLoaded, then
        /// starts the scene's simulation. In client mode the startup-level load is deferred to the join
        /// flow (the level comes from the accept), so this only connects. A missing startup level is a
        /// fatal assert.
        /// @param project  The cooked project MountProjectPacks parsed, naming the startup level.
        void BootstrapWorld(const CookedProject& project);

        /// @brief Runs OnResolveGraphics over a render look composed onto the given surfaces.
        ///
        /// Builds the authored baseline (@p authored mapped onto @p settings and @p view, plus the
        /// primary viewport's current dynamic-resolution choice), invokes OnResolveGraphics once
        /// with @p store's chosen values, and returns the output unapplied. The shared front half of
        /// ApplyGraphicsSettings and the look resolver the compositor hands every viewport.
        /// @param store     The graphics store supplying the chosen values.
        /// @param authored  The authored look to compose with.
        /// @param settings  The topology the baseline starts from.
        /// @param view      The per-frame view knobs the baseline starts from.
        /// @return The resolved renderer state.
        [[nodiscard]] GraphicsResolveOutput
        ResolveGraphicsOutput(const GraphicsSettings& store, const RenderLook& authored,
                              const Renderer::SceneRendererSettings& settings,
                              const Renderer::ViewState& view);

        /// @brief The managed half of ApplyGraphicsSettings: each managed viewport's render scale and
        ///        dynamic resolution, resolved against its scene's look, and the primary's Global facet.
        /// @param display  The built-in display selections the apply writes the calibration from.
        /// @pre The managed set is non-empty and the graphics store exists.
        void ApplyGraphicsToManagedViewports(const BuiltinDisplayChoices& display);

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
        /// bindings into pins here. A world that has left every viewport's applied binding and is still
        /// open reaches OnWorldDeparted first, before it is unpinned.
        void SyncPresentationPins();

        /// @brief Stamps each held scene's presentation scope with the rank of the first registered
        ///        viewport presenting it, for this frame's PresentationScopes::Resolve to latch.
        ///
        /// The rank is the viewport's position in the compositor's registration order, so the
        /// primary viewport presents rank 0 and an overlay registered after it a higher one; the
        /// music arbitration breaks priority ties with it. A viewport's retained scene is compared
        /// against the runner's live scenes by address and never dereferenced, since a viewport may
        /// still retain a scene its world has since closed.
        void StampPresentationRanks();

        /// @brief Fires OnWorldArrival for any pending travel whose rebind has now landed.
        ///
        /// Run once per frame right after the rebinds are applied: a pending arrival whose viewport
        /// now shows its destination has arrived (fire and drop it), one whose viewport is no longer
        /// even heading there was superseded or abandoned (drop it silently), and one still in flight
        /// is left for a later frame.
        void FireWorldArrivals();

        /// @brief Drains the frame's completed and abandoned rebinds into their consumer hooks.
        ///
        /// Run once per frame beside FireWorldArrivals, after the managed set's apply pass has
        /// settled: each completed rebind reaches OnWorldPresented with the seat it ended on, each
        /// abandonment reaches OnWorldPresentAbandoned. The drain empties the set's queues first, so a
        /// hook that records a further rebind is recording against a clean queue.
        void FirePresentationHooks();

        /// @brief Hides the OS cursor while a presented document draws its own, and restores it after.
        ///
        /// Run at the pre-tick input point, once the immediate-mode layer has begun its frame so its
        /// mouse claim is this frame's. Ownership is explicit and one-way: the engine writes cursor
        /// visibility from the first frame a drive-list viewport reports
        /// Renderer::Viewport::IsDrawingCursor, and releases it — back to visible — on the frame the
        /// last such viewport stops. An application that never presents one is never written to, so a
        /// consumer managing the cursor itself simply authors no GuiOverlay::DrawsCursor.
        void ApplyCursorRule();

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

        /// @brief The consumer's judgement on a travel, resolved once from the policy or the hook.
        ///
        /// Held here rather than on the directory because it is asked on the **authoritative** side
        /// of a travel and a travel has two of those: this process for its own player, and the
        /// ServerHost for a connected client's. Both read this one closure.
        function<optional<string>(const Net::JoinRequestInfo&)> m_AuthorizeTravel;

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

        /// @brief Reconciles every live world's LocalControl markers against the presenting viewports.
        ///
        /// The engine's one implementation of "which pawn is mine?" (see Veng/Scene/LocalControl.h):
        /// for each live world it collects the seats the managed viewports present it through and
        /// reconciles that world's markers against them, so a world no viewport presents is cleared
        /// and a split-screen world keeps one marker per presenting viewport. It runs once per frame
        /// after the sim ticks and the net pump, so a possession granted in this frame's Sim and one
        /// that arrived in this frame's snapshot are both marked before the frame renders.
        ///
        /// It is a sweep rather than an event because possession has no engine-side event to listen
        /// to: Possesses is a plain component a game writes directly, and on a client it changes
        /// through snapshot apply. Its cost is the presenting-viewport count per world, never a scan
        /// of a scene's entities. Each marker move raises the marker's event form through
        /// NotifyPossession.
        void SyncLocalControl();

        /// @brief Raises OnClientPossession for a marker move, suppressing a repeat within the frame.
        ///
        /// The single site the hook fires from, so the engine keeps one answer to the question rather
        /// than two that can drift. The client join drive reports its own seat's possession here as it
        /// pumps, and the frame's marker sweep reports every mode's; a scene already notified of the
        /// same pawn this frame is suppressed, so a client sees the transition once. The per-frame
        /// record clears at the end of SyncLocalControl.
        /// @param world  The scene the possession changed in.
        /// @param pawn   The pawn now controlled there, or Entity::Null when none is.
        void NotifyPossession(Scene& world, Entity pawn);

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
        /// while captured with no associated viewport it falls back to the primary world. Handed to
        /// the frame's SimInputFrame before the worlds tick, so only the owning scene's simulation — a
        /// runner world or a driver outside it — sees the pointer.
        /// @return The routing and the scene it applies to, or an empty routing scoped to no scene.
        [[nodiscard]] ScopedPointer ComputePointerRouting() const;

        /// @brief The WorldRunner's context factory: builds the SystemContext a request describes.
        ///
        /// Fills every service, stamps the world (@p request.World) and its authority role
        /// (RoleForWorld), the tick, alpha and step edges from the request, GameplayFocused from the
        /// router, and IsReplay for a Replay request. Pointer is this frame's routing scoped to the
        /// request's scene for a live Sim step or View pass, and empty otherwise. View and Debug
        /// resolve from the scene's primary presenting viewport — the first registered Presented
        /// viewport whose retained scene is the request's — and are nullopt and null for a view-less
        /// scene.
        /// @param request  The world, scene, phase and step the context is for.
        /// @return The assembled context.
        [[nodiscard]] SystemContext MakeSystemContext(const SystemContextRequest& request) const;

        ApplicationInfo m_Info;

        /// @brief Command-line arguments parsed once in Run, before Initialize.
        LaunchArguments m_LaunchArgs;

        /// @brief Borrowed from the host; must outlive this app and every Scene it creates.
        TypeRegistry& m_TypeRegistry;

        /// @brief Borrowed from the host; must outlive this app and every SceneSimulation it drives.
        SystemRegistry& m_SystemRegistry;

        /// @brief Borrowed host-owned GuiDriver catalog, or null; must outlive this app if set.
        GuiDriverRegistry* m_GuiDriverRegistry = nullptr;

        /// @brief The CPU profiler, installed active at construction; the first owned member.
        ///
        /// Declared ahead of every other owned member so it destructs last — after the task system,
        /// so a worker detaching from it always reaches a live profiler, honoring the contract that a
        /// profiler outlives every thread that registered with it. Its constructor registers the
        /// calling (main) thread.
        Diagnostics::Profiler m_Profiler;

        /// @brief Process CPU/memory sampler; sampled once per frame, read via GetSystemStats().
        SystemStatsSampler m_SystemStats;

        /// @brief The virtual GPU track the bridge emits back-dated pass timings onto; 0 until created.
        Diagnostics::TrackId m_GpuTrack = 0;

        /// @brief The bridge's whole-frame GPU scope name, interned with the track; 0 until then.
        Diagnostics::NameId m_GpuFrameName = 0;

        /// @brief Each GPU pass name the bridge has interned, so a frame's passes cost a lookup each
        /// rather than a pass through the profiler's locked string table.
        unordered_map<string, Diagnostics::NameId> m_GpuPassNames;

        /// @brief Per frame-in-flight slot: the profiler frame index whose GPU work occupies the slot.
        ///
        /// The readback in a frame's Context::BeginFrame reports the frame that last used this slot,
        /// N frames ago; the bridge stamps its events with the remembered value and then records the
        /// current frame as the slot's new occupant. Sized to the frames-in-flight count on first use.
        vector<u64> m_GpuSlotFrame;

        /// @brief Per frame-in-flight slot: the trace-clock tick the slot's GPU frame is anchored at.
        vector<u64> m_GpuSlotAnchorTicks;

        /// @brief The application window; null when Headless.
        ///
        /// First of the ordered lifetime members below: they are declared so reverse-declaration
        /// destruction runs the teardown sequence — each releases while every service it borrows is
        /// still alive. Run() ends at its operations (quiesce, OnShutdown, SaveAll, closing every
        /// world) and does no explicit release; destruction owns the release.
        Unique<Window> m_Window;

        /// @brief Frame-coherent input; borrows m_Window, so declared after it (destructs first).
        Unique<Input> m_Input;

        /// @brief The gamepad device layer; null when Headless.
        Unique<GamepadBackend> m_Gamepads;

        /// @brief The slot-indexed pad state the backend writes each frame, kept so a pad's name
        ///        reuses its storage rather than allocating per frame.
        std::array<GamepadState, Input::MaxGamepads> m_GamepadStates;

        Renderer::Context m_RenderContext;

        /// @brief Worker pool; declared before m_AssetManager so it destructs after it — the workers
        ///        stop last, since a live load worker holds Context& and AssetManager state.
        Unique<TaskSystem> m_TaskSystem;

        /// @brief Owns every cached asset; borrows m_RenderContext, so declared after it and destructs
        ///        first, retiring each asset's GPU resources while the context is still live.
        Unique<AssetManager> m_AssetManager;

        /// @brief The runtime glyph rasterizer the shared atlas draws from; declared before the atlas
        ///        (which borrows it) and after m_RenderContext, so it outlives the atlas and both
        ///        tear down before the context.
        Unique<Text::GlyphSource> m_GlyphSource;

        /// @brief The one dynamic glyph atlas, shared by every font; borrows m_RenderContext and
        ///        m_GlyphSource, so it destructs before either.
        Unique<Text::GlyphAtlas> m_GlyphAtlas;

        /// @brief Keeps the graphics-quality schema resident for the settings store to borrow; empty
        ///        when ApplicationInfo::GraphicsSchema is unset. Declared before m_GraphicsSettings so
        ///        the schema outlives the store that points at it.
        AssetHandle<GraphicsSchema> m_GraphicsSchemaHandle;

        /// @brief The per-machine graphics-settings store; borrows the schema handle above and the
        ///        type registry, so it destructs before them.
        Unique<GraphicsSettings> m_GraphicsSettings;

        /// @brief Keeps the audio-settings schema resident for the audio store to borrow; empty when
        ///        ApplicationInfo::AudioSettingsSchema is unset. Declared before m_AudioSettings so
        ///        the schema outlives the store that points at it.
        AssetHandle<SettingsSchema> m_AudioSchemaHandle;

        /// @brief The per-machine audio-settings store; null when no audio schema was named. Borrows
        ///        the schema handle above and the type registry, so it destructs before them.
        Unique<SettingsStore<SettingsChoices>> m_AudioSettings;

        /// @brief Keeps the locale index resident for the localization service to read at boot; empty
        ///        when ApplicationInfo::LocaleIndex is unset.
        AssetHandle<Localization::LocaleIndex> m_LocaleIndexHandle;

        /// @brief The per-machine language store (locale.json); null when no locale index was named.
        Unique<SettingsStore<SettingsChoices>> m_LanguageSettings;

        /// @brief The always-owned localization service: index-backed when a LocaleIndex is named,
        ///        else the inert null-object. Borrows the AssetManager, so it destructs before it.
        Unique<Localization::Localization> m_Localization;

        /// @brief The GuiTranslator adapter over m_Localization, installed on every managed viewport.
        ///
        /// Reads GetLocalization() live so it tracks the service across the null-object → index-backed
        /// swap at boot. Held as the Gui interface so Application.h names no adapter type.
        Unique<Gui::GuiTranslator> m_GuiTranslator;

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

        /// @brief The context the role resolution reads navigation from (SetDefaultUiContext); a
        ///        handle into m_AssetManager, so declared after it.
        AssetHandle<InputMappingContext> m_DefaultUiContext;

        /// @brief Resolves every seat's role-tagged actions each frame and dispatches their presses
        ///        through the router, after the frame's input lands and before any world ticks.
        Unique<RoleResolver> m_RoleResolver;

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

        /// @brief The presentation scopes of every scene the runner holds, plus the application scope.
        ///
        /// Declared before m_WorldRunner so it outlives every scene the runner destroys: each scene's
        /// scope closes into it as the scene goes.
        PresentationScopes m_PresentationScopes;

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

        /// @brief The audio subsystem; the device, the mixing thread, and the bus tree.
        ///
        /// Declared after the asset manager and world runner so it destructs before them: its
        /// destructor stops and joins the mixing thread, so the real-time callback is quiesced
        /// before any clip or generator a voice may reference is freed. Constructed in Initialize
        /// with a null backend when Headless, and pumped once per frame.
        Unique<Audio::AudioDevice> m_AudioDevice;

        /// @brief The haptics engine, mixing every scope's rumble into the pads' motors.
        ///
        /// Its one-shots hold clip handles, so it is declared after the asset manager and destructs
        /// before it; it judges them by m_PresentationScopes, declared ahead of it. Constructed in Initialize and updated once per frame in the presentation step
        /// after OnUpdate.
        Unique<Haptics::HapticsEngine> m_Haptics;

        /// @brief The video recorder, recording the presented frame through the platform's encoder.
        ///
        /// Borrows the render context, the compositor and the audio device, so it is declared after
        /// all three and destructs before them — a running capture is drained and its file committed
        /// while the compositor it is installed on and the device it taps are still alive.
        Unique<Capture::VideoRecorder> m_VideoRecorder;

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

        /// @brief The game's registered net policy (SetNetPolicy); null runs the GameNetInfo closures.
        ///
        /// Borrowed, never owned. Read at the hook consumption points (the bootstrap identity
        /// resolution, the directory/session-registry build, each host mount), all of which run
        /// after OnInitialize — which is what makes OnInitialize the registration deadline.
        GameNetPolicy* m_NetPolicy = nullptr;

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

        /// @brief The worlds the managed viewports' applied bindings presented at the last SyncPresentationPins.
        ///
        /// Pending rebind destinations are excluded, so a world leaving this set is a departure from
        /// a world that was actually presented, never an abandoned destination.
        std::unordered_set<u64> m_PresentedWorlds;

        /// @brief The request-driven focus tokens the FocusRequest drain owns.
        ///
        /// One token per seat a system has captured gameplay focus for through a FocusRequest; the
        /// engine holds it across frames on the stampers' behalf (they cannot) and pops it when a
        /// UI FocusRequest or a ReleaseFocus press releases the seat. A FocusToken is a plain id, so
        /// dropping the list is inert; the router owns the actual focus stack, and says which seat
        /// each token is on.
        vector<FocusToken> m_FocusRequestTokens;

        /// @brief The request-driven world pauses the PauseRequest drain holds, keyed by world id.
        ///
        /// At most one per world, held across frames on the stampers' behalf (they cannot hold a
        /// scope) and dropped by a Paused = false request or by the world's close. Declared after
        /// m_WorldRunner so each scope is destroyed while the runner it releases through still lives.
        unordered_map<u64, WorldPauseScope> m_PauseRequestScopes;

        /// @brief Each world's request policy (SetWorldRequestPolicy), keyed by world id.
        ///
        /// A world absent here drains as Full; an entry is dropped by the world's close.
        unordered_map<u64, WorldRequestPolicy> m_RequestPolicies;

        /// @brief The overlays the scenes' LevelOverlay components opened, reconciled each frame.
        ///
        /// Declared after the runner, the router, the compositor and the managed set, so it is
        /// destroyed while each service an unwind touches still lives.
        Unique<OverlayWorlds> m_Overlays;

        /// @brief A presenting travel awaiting its rebind, so OnWorldArrival can fire when it lands.
        ///
        /// Recorded by Travel when it issues the present-on-ready rebind and drained each frame once
        /// the viewport binding flips to the destination (or dropped when a later travel of the same
        /// viewport supersedes it, or the rebind is abandoned). Carries the payload the arrival hook
        /// hands back and whether the destination was a reused live world.
        struct PendingArrival
        {
            /// @brief The managed viewport the travel presents on.
            usize Index = 0;
            /// @brief The destination world the rebind is landing on.
            WorldInstanceId World;
            /// @brief The travel's arrival payload, handed to OnWorldArrival.
            Net::Blob Payload;
            /// @brief True when the destination was an already-live world this travel reused.
            bool Reused = false;
        };

        /// @brief Presenting travels whose rebind has not yet landed; drained in FireWorldArrivals.
        vector<PendingArrival> m_PendingArrivals;

        /// @brief Scratch for the frame's completed rebinds, reused across frames.
        vector<PresentedViewport> m_PresentedViewports;

        /// @brief Scratch for the frame's abandoned present-on-ready rebinds, reused across frames.
        vector<AbandonedPresent> m_AbandonedPresents;

        /// @brief Whether the engine currently owns the OS cursor's visibility for a drawn-cursor document.
        ///
        /// Retained because the release is an edge: without it the rule could not tell "no document
        /// draws a cursor, and none ever did" (leave the consumer's cursor alone) from "the last one
        /// just went away" (restore visibility).
        bool m_CursorOwned = false;

        /// @brief Scratch for one world's presenting seats, reused across the marker sweep's worlds.
        vector<Entity> m_PresentingSeats;

        /// @brief Scratch for one world's marker moves, reused across the marker sweep's worlds.
        vector<LocalControlChange> m_LocalControlChanges;

        /// @brief The (scene, pawn) possessions already notified this frame; cleared each sweep.
        ///
        /// The client join drive and the marker sweep both report the same transition on a client —
        /// the drive as the stream binds the own seat, the sweep as the marker follows it — so this
        /// keeps OnClientPossession firing once per transition. Bounded by the frame's marker moves.
        vector<std::pair<const Scene*, Entity>> m_PossessionNotices;

        /// @brief The built-in display selections last applied, or nullopt before the first apply.
        ///
        /// ApplyBuiltinDisplay diffs against this so an apply performs only the changes that moved; the
        /// first apply (nullopt) forces the present mode and frame cap so the persisted defaults take.
        optional<BuiltinDisplayChoices> m_ActiveDisplay;

        /// @brief The window's fullscreen mode as of the previous frame, or nullopt before the first
        ///        observation.
        ///
        /// Seeded on the first frame and compared each frame after, so a change is detected as an
        /// event. It is how a user-driven native full-screen toggle (the macOS green title-bar
        /// button) is caught and written back into the persisted store — see SyncUserFullscreenChange.
        optional<FullscreenMode> m_ObservedFullscreen;

        /// @brief The run-loop frame-rate cap; honored once per frame, independent of present-mode vsync.
        ///
        /// Skipped entirely while the frame clock is driven — sleeping to a real-time deadline in a
        /// mode that is not paced by real time would only lengthen already-slow frames.
        FrameRateLimiter m_FrameLimiter;

        /// @brief A frame-clock mode change awaiting the next frame boundary.
        ///
        /// Set present by DriveFrameClock (carrying the descriptor) or ReleaseFrameClock (carrying
        /// nullopt), and consumed at the top of Frame so a request made mid-frame never splits one
        /// frame across two modes.
        optional<optional<FrameClockInfo>> m_PendingFrameClock;

        /// @brief This frame's interpolation fraction (GetSimAlpha), retained for the view pushes.
        f32 m_SimAlpha = 0.0f;

        /// @brief The frame's Sim input protocol, fed by the runner's tick.
        ///
        /// Closed at the top of the next frame: the edges hold after a frame that had a live
        /// simulation but ran no tick, and the Sim deltas drop after one with no live simulation (an
        /// editor with no play session, a full pause). Carries the frame's pointer routing to every
        /// context the world tick builds.
        SimInputFrame m_SimInput;

        bool m_ShouldExit = false;

        /// @brief The status Run returns; 0 until RequestExit(i32) names a failure.
        i32 m_ExitStatus = 0;
    };
}
