#pragma once

#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/Prefab.h>
#include <Veng/Net/Client.h>
#include <Veng/Net/ClockSync.h>
#include <Veng/Net/Interest.h>
#include <Veng/Net/NetEvents.h>
#include <Veng/Net/PredictionHistory.h>
#include <Veng/Net/Reconciliation.h>
#include <Veng/Net/Replication.h>
#include <Veng/Net/Server.h>
#include <Veng/Result.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Veng.h>

#include <span>

// Veng/Net/Host.h — the world glue that makes a connection a seat and join a sequence.
//
// The lifecycle layer (Server/Client) and the replication layer (ReplicationServer/Client) are
// world-agnostic — they move bytes and diff scenes. ServerHost and ClientHost are the thin policy
// objects that bind them to a game world: the server spawns a seat entity per connection and gates
// its replication stream on readiness; the client loads the accepted level, acks, applies the
// spawn stream, and wires its local presentation to its own replicated seat. Both own their
// replication half; a custom app can own them directly, and Plan 07 mounts them in Application.
// Socket-free — every socket type stays behind the Transport seam.

namespace Veng
{
    class Scene;
    class AssetManager;

    /// @brief Configuration for a ServerHost: the underlying server plus the world it seats into.
    struct ServerHostInfo
    {
        /// @brief The underlying Net::Server configuration (transport, connection timing, parity).
        ///
        /// The host installs its own OnAccept (spawning the seat and naming the accept's join
        /// payload); any OnAccept set here is overwritten.
        Net::ServerInfo Server;
        /// @brief The server scene the host spawns each connection's seat into; must outlive the host.
        Scene& World;
        /// @brief The asset manager the seat prefab's dependencies resolve through at spawn.
        AssetManager& Assets;
        /// @brief The AssetId of the level accepted clients load (named in each ConnectAccept).
        AssetId LevelId;
        /// @brief The seat template spawned per connection; null spawns a bare Viewer+Possesses seat.
        Ref<Prefab> SeatPrefab;
        /// @brief If valid, each seat's Spawn rides this prefab id so the client instantiates it too.
        AssetId SeatPrefabId;
        /// @brief The replication cadence for the owned ReplicationServer.
        ReplicationServer::Settings Replication;
        /// @brief The per-connection interest filter; Radius 0 (the default) replicates the whole world.
        Net::InterestSettings Interest;
        /// @brief The game hook adding entities to each connection's interest set; unset adds none.
        Net::InterestPolicy InterestPolicy;
    };

    /// @brief Server-side join glue: a connection becomes a seat; readiness gates its stream.
    ///
    /// On accept the host spawns a Viewer seat entity for the connection — Authority{ Server, Owner =
    /// connectionId } and no SeatInput (the documented remote path) — assigns its wire id, and names
    /// it (with the level) in the ConnectAccept. The game mode's own spawn rule pawns the pawnless
    /// seat with no net awareness. Each Pump generates and sends the replication stream for every
    /// connection that has acked ClientReady, so a client that never readies holds no stream (and is
    /// reaped by the Server's timeout). On disconnect the seat is destroyed and the event surfaced;
    /// pawn cleanup is a game rule over that event.
    class VE_API ServerHost
    {
    public:
        /// @brief Creates a server host, opening its underlying Net::Server.
        /// @param info  Host + server configuration.
        /// @return The host, or an error string if the server's transport could not be opened.
        static Result<Unique<ServerHost>> Create(const ServerHostInfo& info);

        ~ServerHost();

        ServerHost(const ServerHost&) = delete;
        ServerHost& operator=(const ServerHost&) = delete;

        /// @brief Pumps one frame: send this tick's stream to ready seats, then advance the server.
        ///
        /// Assigns wire ids to any newly spawned authoritative entities (the pawns the game's spawn
        /// rule added), generates and queues each ready connection's replication messages, pumps the
        /// transport (flushing those sends, accepting new connections — which spawns their seats —
        /// and reaping dead ones), tears down a disconnected connection's seat, and folds a
        /// ClientReady into the readiness gate. The caller sets the scene's change tick for @p tick
        /// before calling.
        /// @param now   Monotonic time in seconds (injected).
        /// @param tick  The current server sim tick (the snapshot cadence and header time).
        void Pump(f64 now, u64 tick);

        /// @brief The underlying server (for LocalPort, Connections, an app's own traffic).
        [[nodiscard]] Net::Server& Server();

        /// @brief The owned replication server.
        [[nodiscard]] ReplicationServer& Replication();

        /// @brief The wire-id allocator the host assigns seats and authoritative entities from.
        [[nodiscard]] NetIdAllocator& Allocator();

        /// @brief The seat entity spawned for a connection, or Entity::Null for an unknown id.
        [[nodiscard]] Entity SeatFor(Net::ConnectionId id) const;

        /// @brief Whether a connection has acked ClientReady (its stream is flowing).
        [[nodiscard]] bool IsReady(Net::ConnectionId id) const;

        /// @brief The lifecycle events surfaced this Pump, for game policy (e.g. pawn cleanup).
        /// @return A view valid until the next Pump.
        [[nodiscard]] std::span<const Net::NetEvent> Events() const;

    private:
        struct State;

        explicit ServerHost(Unique<State> state);

        Unique<State> m_State;
    };

    /// @brief Configuration for a ClientHost: the connection plus the world-load and wiring hooks.
    struct ClientHostInfo
    {
        /// @brief The connection to the server; must outlive the host.
        Net::Client& Client;
        /// @brief The asset manager a replicated prefab spawn resolves through.
        AssetManager& Assets;
        /// @brief Loads the accepted level into a fresh client Scene, with authoritative entities skipped.
        ///
        /// Invoked once, when the accept arrives, with the level's AssetId — the app returns the
        /// spawned client Scene (in practice Level::LoadInto with SkipServerAuthoritative). The host
        /// owns the returned scene and applies the spawn stream into it.
        function<Unique<Scene>(AssetId)> LoadLevel;
        /// @brief Resolves a replicated spawn's prefab AssetId to a resident Prefab (the spawn arm).
        function<Ref<Prefab>(AssetId)> ResolvePrefab;
        /// @brief Optional: wire the local presentation when the own seat's possessed pawn changes.
        ///
        /// Called with the client scene and the pawn the replicated seat now possesses (Entity::Null
        /// when it possesses none) — the app points its Local-tier camera/viewer at that pawn. The
        /// camera rig itself is untouched Local-tier View machinery; this only names its target.
        function<void(Scene&, Entity)> OnPossession;
        /// @brief Optional: selects the predicted entity set on a possession change; null uses the default.
        ///
        /// On each possession change the host promotes this set from Remote to Predicted and tracks it
        /// in the PredictionHistory, demoting the prior set back to Remote. Unset uses the
        /// owner-pawn-subtree default (see PredictionPolicy).
        PredictionPolicy Prediction;
        /// @brief Optional: replays one predicted Sim tick during rollback reconciliation.
        ///
        /// On a mispredict the host restores the predicted set to the authoritative state and calls
        /// this for each recorded input C+1..now (see Net::ReplayTick): the implementer sets the local
        /// seat's PlayerInput and advances the scene's Sim phase for that tick with
        /// SystemContext::IsReplay set. Unset disables rollback — a mispredict hard-snaps to the
        /// authoritative state (planset-54 behaviour).
        Net::ReplayTick Replay;
        /// @brief The reconciliation compare tolerances and smoothing knobs (defaulted when unset).
        Net::ReconcileTolerances Tolerances;
        /// @brief Tick-offset controller tuning — the sim tick rate and the lead margin.
        ///
        /// The estimator converts RTT/jitter seconds into a tick lead at TickRate, so it must match
        /// the world's SimTickRate; MarginTicks carries the fixed safety lead beyond the round-trip
        /// estimate (the snapshot-cadence staleness plus the buffered-input cushion).
        Net::TickSyncSettings TickSync;
    };

    /// @brief Client-side join glue: load, ready, apply the stream, wire the own seat.
    ///
    /// Once the connection is accepted the host loads the named level (server-authoritative entities
    /// skipped — they arrive from the stream), acks ClientReady, and thereafter applies the reliable
    /// spawn/despawn stream and the unreliable snapshots into the client scene. It watches for its
    /// own seat (named by the accept's SeatNetId) to bind, then keeps the local presentation wired to
    /// that seat's replicated Possesses — respawns and vehicle swaps arrive as ordinary Possesses
    /// state, with no bespoke message.
    ///
    /// On each possession change it also promotes the predicted set (the possessed pawn plus the
    /// subtree the PredictionPolicy selects) from Remote to Predicted and tracks it in the owned
    /// PredictionHistory, demoting the prior set. The authority filter then runs the real Sim systems
    /// for the predicted set client-side each tick, so the local pawn responds on the tick its input
    /// is sampled; RecordPrediction captures the per-tick input and state for reconciliation.
    class VE_API ClientHost
    {
    public:
        /// @brief Creates a client host over an already-connecting Net::Client.
        /// @param info  Host configuration.
        /// @return The host.
        static Unique<ClientHost> Create(const ClientHostInfo& info);

        ~ClientHost();

        ClientHost(const ClientHost&) = delete;
        ClientHost& operator=(const ClientHost&) = delete;

        /// @brief Pumps one frame: advance the connection, load-on-accept, apply the stream, wire the seat.
        /// @param now  Monotonic time in seconds (injected).
        void Pump(f64 now);

        /// @brief The loaded client scene, or nullptr before the accept loads it.
        [[nodiscard]] Scene* World() const;

        /// @brief The owned replication client (its NetId → Entity map, spawn arm).
        [[nodiscard]] ReplicationClient& Replication();

        /// @brief The client's own seat entity once it binds, or Entity::Null.
        [[nodiscard]] Entity Seat() const;

        /// @brief The pawn the own seat currently possesses (as last wired), or Entity::Null.
        [[nodiscard]] Entity PossessedPawn() const;

        /// @brief The prediction history recording the predicted set's per-tick input and state.
        ///
        /// The client's tracked (predicted) set is registered here on each possession change; the
        /// per-tick RecordPrediction captures into it, and reconciliation restores/replays from it.
        [[nodiscard]] Net::PredictionHistory& History();

        /// @brief The prediction history (read-only) — its tracked set and recorded ticks.
        [[nodiscard]] const Net::PredictionHistory& History() const;

        /// @brief Records the predicted set's state and the local seat's input for a client tick.
        ///
        /// Called once per client Sim tick, after the predicted movement has run: captures every
        /// tracked entity's replicated state alongside the local input seat's resolved PlayerInput for
        /// @p tick, so a later reconciliation can restore and replay. A no-op before any pawn is
        /// promoted (an empty tracked set) or before the world loads.
        /// @param tick  The client sim tick whose predicted state and input are recorded.
        void RecordPrediction(u64 tick);

        /// @brief Whether the level has loaded and readiness has been acked.
        [[nodiscard]] bool IsJoined() const;

        /// @brief The highest server sim tick a snapshot has carried, or 0 before the first.
        ///
        /// The tick-offset controller compares the client's own tick against this to size its lead.
        [[nodiscard]] u64 LastServerTick() const;

        /// @brief The client tick-offset controller — smoothed RTT/jitter and the running estimate.
        ///
        /// Fed by ObserveTickSync each frame; read for the target lead and bounded slew. The estimate
        /// is inert until a driver applies the slew to its sim clock.
        [[nodiscard]] const Net::TickOffsetEstimator& TickSync() const;

        /// @brief Folds this frame's link state into the tick-offset controller and returns the slew.
        ///
        /// Observes the connection's smoothed RTT and the freshest server tick against @p clientTick,
        /// updating TickSync(). A no-op returning 1.0 (no slew) before the connection is established
        /// or before any server tick has arrived.
        /// @param clientTick  The client's current sim tick.
        /// @return The bounded step multiplier to apply next tick, or 1.0 when not yet syncing.
        f32 ObserveTickSync(u64 clientTick);

        /// @brief Sets the closed-loop feedback trim on the tick-offset controller, in ticks.
        /// @param trimTicks  The server's consumed-input early/late correction.
        void SetTickSyncFeedback(f32 trimTicks);

    private:
        struct State;

        explicit ClientHost(Unique<State> state);

        Unique<State> m_State;
    };
}
