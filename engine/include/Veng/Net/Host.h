#pragma once

#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/Prefab.h>
#include <Veng/Net/AccountId.h>
#include <Veng/Net/Client.h>
#include <Veng/Net/JoinRequest.h>
#include <Veng/Net/ClockSync.h>
#include <Veng/Net/Interest.h>
#include <Veng/Net/Messages.h>
#include <Veng/Net/NetEvents.h>
#include <Veng/Net/PredictionHistory.h>
#include <Veng/Net/Reconciliation.h>
#include <Veng/Net/Replication.h>
#include <Veng/Net/Server.h>
#include <Veng/Net/TravelPayload.h>
#include <Veng/Net/WorldKey.h>
#include <Veng/Result.h>
#include <Veng/Scene/Entity.h>
#include <Veng/Veng.h>
#include <Veng/World.h>
#include <Veng/WorldDirectory.h>

#include <span>

// Veng/Net/Host.h — the world glue that multiplexes N worlds over one connection.
//
// The lifecycle layer (Server/Client) and the replication layer (ReplicationServer/Client) are
// world-agnostic — they move bytes and diff scenes. ServerHost and ClientHost are the thin policy
// objects that bind them to game worlds. A client joins a world by an opaque WorldKey; the server
// resolves it through a get-or-create factory, assigns a per-connection JoinId, spawns the
// connection's seat in that world, and gates that world's stream on a per-world ClientReady. Every
// world-tagged message rides the world-multiplexing envelope (a JoinId ahead of the payload), so N
// worlds share one connection's two reliability channels.
//
// Replication is per world, owned by the Host: a ServerHost holds one ReplicationServer per hosted
// world, the shared WorldKey → WorldInstanceId map, and a (connection, JoinId) → world binding,
// demuxing each connection's inbound traffic by JoinId to the right world's instance and muxing its
// sends back; a ClientHost owns one ReplicationClient per joined world. Because an instance only
// ever sees its own world's connections and acks, ack-scoping and baseline isolation are structural
// — one world's ack can never advance a peer world's baseline (their wire streams remain coupled on
// the shared channels, so the honest guarantee is convergence, not stream independence). A custom
// app can own the hosts directly; Application mounts them as the plug-and-play path. Socket-free —
// every socket type stays behind the Transport seam.
//
// This layer widens the blast radius of the transport's pre-existing unauthenticated posture: an
// attacker spoofing an established connection's UDP source reaches every world that connection has
// joined, not one. The authorization hook is a policy seam, not a security mechanism; the scope
// stays LAN/trusted, and a transport-security layer is where such a defense would sit.

namespace Veng
{
    class Scene;
    class AssetManager;

    /// @brief One hosted world's scene, seat rule, replication cadence, and content digest.
    ///
    /// A ServerHost hosts one world at Create (from its ServerHostInfo) and any number more through
    /// AddWorld or its get-or-create factory. Each carries its own ReplicationServer, NetId
    /// allocator, seat prefab, level, and interest filter, so its replication state is wholly its
    /// own. The shared Net::Server and asset manager come from the host, not from here.
    struct ServerWorldInfo
    {
        /// @brief The WorldInstanceId this hosted world's replication instance is keyed by.
        WorldInstanceId WorldId;
        /// @brief The opaque key clients present to join this world; keys the shared get-or-create map.
        Net::WorldKey Key;
        /// @brief The server scene the host spawns this world's seats into; must outlive the host.
        Scene& World;
        /// @brief The AssetId of the level a client joining this world loads (named in its join reply).
        AssetId LevelId;
        /// @brief The content digest echoed to a joining client to validate its reconstructed world.
        Net::ContentDigest Digest;
        /// @brief The seat template spawned per join; null spawns a bare Viewer+Possesses seat.
        Ref<Prefab> SeatPrefab;
        /// @brief If valid, each seat's Spawn rides this prefab id so the client instantiates it too.
        AssetId SeatPrefabId;
        /// @brief The replication cadence for this world's ReplicationServer.
        ReplicationServer::Settings Replication;
        /// @brief The per-connection interest filter; Radius 0 (the default) replicates the whole world.
        Net::InterestSettings Interest;
        /// @brief The game hook adding entities to each connection's interest set; unset adds none.
        Net::InterestPolicy InterestPolicy;
    };

    // ServerWorldResolution and WorldPlacement are the get-or-place vocabulary; they live in
    // Veng/WorldDirectory.h (the role-neutral directory that owns the map/placement/reap), and the
    // ServerHost consumes them through it.

    /// @brief Configuration for a ServerHost: the shared server plus its initial hosted world.
    struct ServerHostInfo
    {
        /// @brief The underlying Net::Server configuration (transport, connection timing, parity).
        Net::ServerInfo Server;
        /// @brief The WorldInstanceId of the initial hosted world (its replication instance's key).
        ///
        /// Also the primary world: the one Replication() and Allocator() resolve, registered under Key.
        WorldInstanceId WorldId;
        /// @brief The opaque key the initial world is registered under (the single-world default key).
        Net::WorldKey Key = Net::DefaultWorldKey;
        /// @brief The server scene the host spawns the initial world's seats into; must outlive the host.
        Scene& World;
        /// @brief The asset manager the seat prefab's dependencies resolve through at spawn.
        AssetManager& Assets;
        /// @brief The AssetId of the level accepted clients load (named in the initial world's join reply).
        AssetId LevelId;
        /// @brief The content digest echoed to a client joining the initial world, for validation.
        Net::ContentDigest Digest;
        /// @brief The seat template spawned per join; null spawns a bare Viewer+Possesses seat.
        Ref<Prefab> SeatPrefab;
        /// @brief If valid, each seat's Spawn rides this prefab id so the client instantiates it too.
        AssetId SeatPrefabId;
        /// @brief The replication cadence for the initial world's ReplicationServer.
        ReplicationServer::Settings Replication;
        /// @brief The per-connection interest filter; Radius 0 (the default) replicates the whole world.
        Net::InterestSettings Interest;
        /// @brief The game hook adding entities to each connection's interest set; unset adds none.
        Net::InterestPolicy InterestPolicy;
        /// @brief The most worlds one connection may join; a join past it is denied PerConnectionCapReached.
        u32 MaxJoinedWorldsPerConnection = 4;
        /// @brief The server-wide bound on total live worlds; a create-on-miss past it is denied HostedWorldsCapReached.
        u32 MaxHostedWorlds = 64;
        /// @brief Per-instance seat cap the built-in placement policy fills to; 0 (the default) means no cap.
        ///
        /// Drives the built-in get-or-place policy when Placement is unset: 0 converges every joiner of a
        /// key on one bucket (byte-identical to a single get-or-create instance), and a value > 0 places a
        /// joiner into the first bucket with fewer than this many live seats, opening a fresh bucket through
        /// the WorldFactory when every existing one is full. Ignored when Placement is set.
        u32 MaxPlayersPerInstance = 0;
        /// @brief Seconds a world with no live joins is held warm before it is reaped (CloseWorld).
        f64 IdleKeepWarmDwell = 5.0;
        /// @brief The authorization hook: may this requester join/create this key? Unset allows all.
        ///
        /// Called before any cap check, world open, or JoinId assignment, so a refusal leaves no
        /// resource to reap. The request identity carries the connection, its admitted account (always
        /// valid — admission precedes authorization), the key, and the opaque travel payload, so a
        /// policy may gate on who is asking or on arrival data. A policy seam, not a security
        /// mechanism (the transport is unauthenticated).
        function<bool(const Net::JoinRequestInfo&)> Authorize;
        /// @brief The get-or-create factory: materialize a world for a key that missed the shared map.
        ///
        /// Called only on a miss, after the caps clear, to open a new world through the consumer's
        /// runner; returning nullopt denies the join NoSuchWorld. Unset means only pre-registered
        /// worlds (Create + AddWorld) can be joined. A hit reuses the existing instance, so two
        /// connections presenting the same key converge on one shared world. The travel payload rides in
        /// so a world may be parameterized by data no key encodes.
        function<optional<ServerWorldResolution>(const Net::WorldKey&, const Net::TravelPayload&)>
            WorldFactory;
        /// @brief The get-or-place policy: which live bucket of a key a joiner lands in, or a fresh one.
        ///
        /// Called on every join after authorize + the per-connection cap clear, with the request
        /// identity (connection, account, key, payload) and the key's live buckets (each with its
        /// presence and recorded payload). Returning an offered bucket's id places the joiner there
        /// (converging on it); returning nullopt asks for a fresh bucket, opened through WorldFactory
        /// and bounded by MaxHostedWorlds. Unset uses the built-in capacity policy driven by
        /// MaxPlayersPerInstance (convergence when that is 0). The payload lets a proximity policy
        /// match a request against every live bucket's params; party/affinity grouping is not
        /// expressed here.
        function<optional<WorldInstanceId>(const Net::JoinRequestInfo&,
                                           std::span<const WorldPlacement>)>
            Placement;
        /// @brief Closes a factory-opened world when it idles out; unset leaves the world open.
        ///
        /// Invoked with a factory-opened world's id once it has been join-less past IdleKeepWarmDwell,
        /// so the consumer can capture its persistent state; the world's runner teardown follows. Pre-
        /// registered worlds (Create + AddWorld) are never reaped — the consumer owns their lifetime.
        function<void(WorldInstanceId)> CloseWorld;
        /// @brief The local player's account (a listen host's own player); invalid for a host with none.
        ///
        /// An account-addressed message send resolving to this account loops back to the host's own
        /// registered channel handler, connection-free — the listen host's player is a first-class
        /// message recipient beside connected ones. A dedicated host resolves no local account and
        /// leaves it invalid.
        Net::AccountId LocalAccount;
        /// @brief The directory the host consumes for get-or-place + lifetime; unset builds one from the hooks.
        ///
        /// When set (borrowed, must outlive the host), the host resolves joins and reports presence
        /// through this shared directory rather than owning the map/refcount/reap — the path an
        /// Application takes to share one directory across its standalone travel and its hosting. When
        /// unset, the host builds a private directory from the caps and policy hooks above, so a
        /// stand-alone ServerHost keeps its self-contained behaviour.
        WorldDirectory* Directory = nullptr;
    };

    /// @brief Server-side join glue: a connection joins worlds by WorldKey; readiness gates each stream.
    ///
    /// On a join request the host resolves the opaque WorldKey in a fixed order — authorize, per-
    /// connection cap, server-wide cap, get-or-create through the factory — then assigns a
    /// per-connection JoinId, spawns a Viewer seat in that world (Authority{ Server, Owner = id }, no
    /// SeatInput — a remote seat's input arrives from the wire), and replies with the world's level,
    /// a content digest, and the seat's wire id. The game mode's own spawn rule pawns the pawnless
    /// seat with no net awareness. Each Pump generates and sends the replication stream for every
    /// (connection, join) that has acked its per-world ClientReady, demuxing inbound datagrams by
    /// JoinId to the right world's ReplicationServer and dropping a datagram whose JoinId the
    /// connection was never granted. On disconnect every join's seat is destroyed and the event
    /// surfaced; a world whose last join leaves is held warm then reaped.
    class VE_API ServerHost
    {
    public:
        /// @brief Creates a server host, opening its underlying Net::Server and its initial world.
        /// @param info  Host + server configuration.
        /// @return The host, or an error string if the server's transport could not be opened.
        static Result<Unique<ServerHost>> Create(const ServerHostInfo& info);

        ~ServerHost();

        ServerHost(const ServerHost&) = delete;
        ServerHost& operator=(const ServerHost&) = delete;

        /// @brief Pre-registers an additional world under its key, with its own ReplicationServer.
        ///
        /// The world becomes joinable by its key: a join request naming it spawns the connection's
        /// seat into its scene and streams from its replication instance. A pre-registered world is
        /// never idle-reaped (the consumer owns its lifetime); registering a key already mapped
        /// replaces its configuration.
        /// @param world  The world's key, scene, level, seat rule, replication cadence, and interest.
        void AddWorld(const ServerWorldInfo& world);

        /// @brief Pumps one frame: send each ready join's stream, then advance the server.
        ///
        /// Assigns wire ids to newly spawned authoritative entities per world, generates and queues
        /// each ready (connection, join)'s replication messages (each wrapped in its JoinId envelope),
        /// pumps the transport, resolves inbound join requests (authorize → caps → get-or-create →
        /// seat + JoinId + reply), folds each per-world ClientReady into its readiness gate, tears down
        /// a disconnected connection's seats, and reaps idle factory worlds. The caller sets each
        /// world's change tick for @p tick before calling.
        /// @param now   Monotonic time in seconds (injected).
        /// @param tick  The current server sim tick (the snapshot cadence and header time).
        void Pump(f64 now, u64 tick);

        /// @brief The underlying server (for LocalPort, Connections, an app's own traffic).
        [[nodiscard]] Net::Server& Server();

        /// @brief The primary hosted world's replication server (the initial world's instance).
        [[nodiscard]] ReplicationServer& Replication();

        /// @brief The replication server for the world a connection's current (first) join resolves to.
        ///
        /// The current-join convenience: a single-join connection's one world. Falls back to the
        /// primary world's instance for a connection with no joins.
        /// @param id  The connection whose current join's replication is resolved.
        [[nodiscard]] ReplicationServer& ReplicationFor(Net::ConnectionId id);

        /// @brief The replication server for a specific (connection, join) — the inbound demux target.
        /// @param id    The connection.
        /// @param join  The JoinId (granted to @p id); falls back to the primary world if ungranted.
        [[nodiscard]] ReplicationServer& ReplicationForJoin(Net::ConnectionId id, Net::JoinId join);

        /// @brief The replication server for a specific hosted world.
        /// @param world  A hosted world's id (from Create, AddWorld, or the factory).
        [[nodiscard]] ReplicationServer& ReplicationForWorld(WorldInstanceId world);

        /// @brief The world a connection's current (first) join resolves to, or an invalid id if none.
        /// @param id  The connection to resolve.
        [[nodiscard]] WorldInstanceId WorldFor(Net::ConnectionId id) const;

        /// @brief The world a specific (connection, join) resolves to, or an invalid id if ungranted.
        /// @param id    The connection.
        /// @param join  The JoinId to resolve.
        [[nodiscard]] WorldInstanceId WorldForJoin(Net::ConnectionId id, Net::JoinId join) const;

        /// @brief The primary hosted world's wire-id allocator (the initial world's instance).
        [[nodiscard]] NetIdAllocator& Allocator();

        /// @brief The wire-id allocator for a specific hosted world.
        /// @param world  A hosted world's id.
        [[nodiscard]] NetIdAllocator& AllocatorForWorld(WorldInstanceId world);

        /// @brief The seat entity for a connection's current (first) join, or Entity::Null.
        /// @param id  The connection to resolve.
        [[nodiscard]] Entity SeatFor(Net::ConnectionId id) const;

        /// @brief The seat entity for a specific (connection, join), or Entity::Null if ungranted.
        /// @param id    The connection.
        /// @param join  The JoinId to resolve.
        [[nodiscard]] Entity SeatFor(Net::ConnectionId id, Net::JoinId join) const;

        /// @brief The account bound to a connection at admission, or the invalid id when unknown.
        /// @param id  The connection to resolve.
        [[nodiscard]] Net::AccountId AccountFor(Net::ConnectionId id) const;

        /// @brief The live connection an account is bound to, or ServerConnectionId when none is.
        ///
        /// Exactly one live connection holds an account (a duplicate is refused at the handshake), so
        /// the reverse lookup is single-valued; after a reconnect it re-points to the fresh connection.
        /// @param account  The account to resolve.
        [[nodiscard]] Net::ConnectionId ConnectionFor(const Net::AccountId& account) const;

        /// @brief The JoinId of a connection's current (first) join, or ControlJoinId if it has none.
        /// @param id  The connection to resolve.
        [[nodiscard]] Net::JoinId CurrentJoin(Net::ConnectionId id) const;

        /// @brief The JoinIds a connection has been granted, in ascending order.
        /// @param id  The connection to resolve.
        /// @return The granted JoinIds, empty for a connection with no joins.
        [[nodiscard]] vector<Net::JoinId> JoinsFor(Net::ConnectionId id) const;

        /// @brief Whether a JoinId was granted to a connection (the inbound-demux gate).
        /// @param id    The connection.
        /// @param join  The JoinId to test.
        [[nodiscard]] bool IsGranted(Net::ConnectionId id, Net::JoinId join) const;

        /// @brief Whether a connection's current (first) join has acked its ClientReady.
        /// @param id  The connection to resolve.
        [[nodiscard]] bool IsReady(Net::ConnectionId id) const;

        /// @brief Whether a specific (connection, join) has acked its ClientReady (its stream flows).
        /// @param id    The connection.
        /// @param join  The JoinId to test.
        [[nodiscard]] bool IsReady(Net::ConnectionId id, Net::JoinId join) const;

        /// @brief Directs a connection to travel: join a world by key and, once ready, leave a join.
        ///
        /// Sends a directed-travel control message: the client joins @p key (carrying @p payload through
        /// the ordinary join flow, digest validation included) and, once that join is ready, leaves
        /// @p leave — make-before-break, so a denied join leaves the client where it was. A @p leave of
        /// ControlJoinId names nothing to leave (a fresh travel). Sent automatically as the reply to a
        /// client travel request, and callable unprompted for server-driven placement.
        /// @param connection  The connection to direct.
        /// @param leave       The connection's join to leave once the new one is ready, or ControlJoinId.
        /// @param key         The world the client must join.
        /// @param payload     The opaque travel payload the client carries into the join.
        void DirectTravel(Net::ConnectionId connection, Net::JoinId leave, const Net::WorldKey& key,
                          const Net::TravelPayload& payload);

        /// @brief Registers the handler for a game message channel; one handler per channel.
        ///
        /// Inbound messages addressed to @p channel deliver to @p onMessage at DeliverMessages with
        /// the originating connection (ServerConnectionId for a local loopback delivery — see
        /// ServerHostInfo::LocalAccount) and the opaque blob. Registering an already-registered
        /// channel replaces its handler. A message on a channel with no handler drops with a
        /// one-shot per-channel log.
        /// @param channel    The minted channel id to receive on.
        /// @param onMessage  The handler invoked per delivered message.
        void RegisterChannel(Net::ChannelId channel,
                             function<void(Net::ConnectionId, const Net::Blob&)> onMessage);

        /// @brief Queues a game message to one connection on a channel; fails loudly, never silently.
        ///
        /// Reliable-ordered within the live connection, at-most-once across its lifetime: a message
        /// accepted here either arrives in order or the connection has died. Fails with the reason
        /// when the connection is unknown, the payload exceeds Net::MaxMessagePayloadSize (no
        /// fragmentation), or the connection's outbound queue is at its cap
        /// (Net::MaxOutboundMessages / Net::MaxOutboundMessageBytes, whichever first). Queued
        /// messages flush on the next Pump.
        /// @param to       The connection to deliver to.
        /// @param channel  The channel the peer's handler is registered on.
        /// @param payload  The opaque message blob; moved into the queue.
        /// @return Empty on acceptance, or the failure reason.
        VoidResult Send(Net::ConnectionId to, Net::ChannelId channel, Net::Blob payload);

        /// @brief Queues a game message to an account; fails immediately when it is not reachable.
        ///
        /// Resolves the account's live connection (ConnectionFor) and sends there. An account with
        /// no live connection fails with the reason — no engine retry, no offline queue — except
        /// the host's own local account (ServerHostInfo::LocalAccount), which loops back to the
        /// local registered handler, connection-free, delivered at DeliverMessages like any inbound
        /// message.
        /// @param to       The account to deliver to.
        /// @param channel  The channel the recipient's handler is registered on.
        /// @param payload  The opaque message blob; moved into the queue.
        /// @return Empty on acceptance, or the failure reason.
        VoidResult Send(Net::AccountId to, Net::ChannelId channel, Net::Blob payload);

        /// @brief Queues a game message to every member account of a hosted world's key.
        ///
        /// Fans out over the directory's membership (WorldDirectory::MembersOf) for the world's
        /// key — every account present across the key's buckets, the listen host's own player
        /// included (looped back connection-free) — one account-addressed send each. Fails with the
        /// reason when @p world names no hosted world; a member send that fails (an outbound cap)
        /// is folded into the returned error while the remaining members still receive.
        /// @param world    A hosted world's id; its key names the membership.
        /// @param channel  The channel the members' handlers are registered on.
        /// @param payload  The opaque message blob, copied per member.
        /// @return Empty when every member send was accepted, or the failure reason(s).
        VoidResult SendToWorldMembers(WorldInstanceId world, Net::ChannelId channel,
                                      const Net::Blob& payload);

        /// @brief Delivers queued inbound game messages to their registered channel handlers.
        ///
        /// Receipt is frame-safe: Pump only queues inbound messages, and this dispatches them —
        /// call it at a point outside any scene iteration or sim tick (Application calls it at its
        /// top-of-frame request-drain slot). Messages deliver in arrival order per connection. A
        /// message on an unregistered channel drops with a one-shot per-channel log.
        void DeliverMessages();

        /// @brief The number of currently live hosted worlds (pre-registered plus factory-opened).
        [[nodiscard]] usize HostedWorldCount() const;

        /// @brief The lifecycle events surfaced this Pump, for game policy (e.g. pawn cleanup).
        /// @return A view valid until the next Pump.
        [[nodiscard]] std::span<const Net::NetEvent> Events() const;

    private:
        struct State;

        explicit ServerHost(Unique<State> state);

        Unique<State> m_State;
    };

    /// @brief Configuration for a ClientHost: the connection plus the world-load and wiring hooks.
    ///
    /// The hooks apply per joined world (keyed by JoinId); a single-world session auto-joins one key
    /// on connect and drives them through the current-join convenience accessors. A multiplexed
    /// client presents further keys through ClientHost::Join.
    struct ClientHostInfo
    {
        /// @brief The connection to the server; must outlive the host.
        Net::Client& Client;
        /// @brief The asset manager a replicated prefab spawn resolves through.
        AssetManager& Assets;
        /// @brief The world to auto-join on connect (the single-world default key).
        Net::WorldKey WorldKey = Net::DefaultWorldKey;
        /// @brief Whether to auto-join WorldKey once the connection is accepted.
        ///
        /// True is the single-world convenience: the host requests WorldKey the moment it connects. A
        /// client that drives its own joins (through ClientHost::Join) sets this false.
        bool AutoJoin = true;
        /// @brief Computes the digest the client validates against the join reply's echoed world digest.
        ///
        /// Given the WorldKey being joined and the reply's echoed opaque travel payload, returns the
        /// client's own digest of its reconstructed world; a mismatch rejects the join loudly (no
        /// stream is applied). The client mirror of the server's per-key
        /// ServerWorldResolution::Digest — a client joining multiple worlds by opaque key yields a
        /// distinct expected digest per key, and a world parameterized by payload rather than key
        /// (see Net::TravelPayload) folds the echoed payload into it. Unset returns the zero digest,
        /// which matches a content-free server world.
        function<Net::ContentDigest(const Net::WorldKey&, const Net::TravelPayload&)> WorldDigest;
        /// @brief Loads the joined world's level into the caller's client scene, authoritative entities skipped.
        ///
        /// Invoked per join, when the join reply arrives, with the level's AssetId — the app loads the
        /// level (in practice Level::LoadInto with SkipServerAuthoritative) into a scene it owns
        /// elsewhere (a WorldRunner world) and returns a borrowed pointer to it. The host does not own
        /// the scene; it applies the spawn stream into the borrowed one, which must outlive the host.
        /// Null on a load failure. A multiplexed client distinguishes joins by the level id (or the
        /// returned scene) it hands back here.
        function<Scene*(AssetId)> LoadLevel;
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
        /// estimate (the snapshot-cadence staleness plus the buffered-input cushion). Applied to every
        /// joined world's controller.
        Net::TickSyncSettings TickSync;
        /// @brief The spatial dequantization grid every joined world's replication client decodes with.
        ///
        /// Must match the server's ReplicationServer::Settings::Quantization; the host threads the
        /// shared GameNetInfo value onto each join's ReplicationClient as it is created — unless
        /// WorldQuantization overrides it per key (a world with a wider spatial envelope).
        Net::QuantizationSettings Quantization;
        /// @brief Per-key spatial dequantization override; unset uses the shared Quantization on every join.
        ///
        /// The client mirror of the server's already-per-world ServerWorldResolution::Replication: given
        /// the WorldKey being joined, returns the dequantization grid that join's ReplicationClient
        /// decodes with, so two hosted worlds with different spatial envelopes both decode correctly on
        /// one client. Unset threads the shared Quantization onto every join (the single-envelope default).
        function<Net::QuantizationSettings(const Net::WorldKey&)> WorldQuantization;
        /// @brief Optional: closes a join left by a make-before-break directed travel; the caller frees its world.
        ///
        /// Called with the JoinId a directed travel dropped once its make-before-break destination became
        /// ready (the old join is no longer applied), so the caller can close that join's runner world
        /// (WorldRunner::CloseWorld). Unset leaves the caller to observe the leave through Joins().
        function<void(Net::JoinId)> OnLeaveWorld;
        /// @brief Optional: reports a directed-travel destination the server refused; the client stays put.
        ///
        /// Called with the refused key and reason when a make-before-break join is denied — the old join
        /// is never left (the snap-back is "never left"), and this surfaces the reason a caller shows.
        function<void(const Net::WorldKey&, Net::JoinDenyReason)> OnTravelDenied;
    };

    /// @brief Client-side join glue: join by WorldKey, load, ready, apply each world's stream.
    ///
    /// Once the connection is accepted the host requests the auto-join world (unless AutoJoin is off),
    /// naming it by WorldKey. On the join reply it validates the echoed content digest against its own
    /// reconstruction (rejecting a mismatch loudly), loads the named level (server-authoritative
    /// entities skipped — they arrive from the stream), acks a per-world ClientReady, and thereafter
    /// applies that world's reliable spawn/despawn stream and its unreliable snapshots — each demuxed
    /// by its JoinId. It watches for the join's own seat (named by the reply's SeatNetId) to bind,
    /// then keeps the local presentation wired to that seat's replicated Possesses, with no bespoke
    /// message.
    ///
    /// Each joined world keeps its own ReplicationClient, NetId map, prediction history, and
    /// tick-offset controller — identity and clock scope per JoinId. A single-world session drives the
    /// current-join convenience accessors (the one and only join); a multiplexed client keys by
    /// JoinId. On each possession change the join's predicted set (the possessed pawn plus the subtree
    /// the PredictionPolicy selects) is promoted Remote → Predicted and tracked, and RecordPrediction
    /// captures the per-tick input and state for that world's reconciliation.
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

        /// @brief Requests joining a world by its opaque key; the reply assigns its JoinId.
        ///
        /// Queues a join request the next Pump sends (or sends it now if already connected). The join
        /// reply — validated and loaded through the info hooks — assigns the JoinId under which the
        /// world's accessors key. Presenting a key already joined is idempotent. The auto-join issues
        /// this for WorldKey on connect; a multiplexed client calls it per additional world.
        /// @param key      The opaque world to join.
        /// @param payload  The opaque travel payload threaded into the server's resolution; empty by default.
        void Join(const Net::WorldKey& key, const Net::TravelPayload& payload = {});

        /// @brief Requests joining a world *into an existing live scene* — the adopt-in-place join.
        ///
        /// Like Join, but the reply loads no level: @p adoptScene is the client's already-standing
        /// reconstruction of the world, and the stream's spawns apply into it. The echoed content digest
        /// is still validated (fail-loud on mismatch, before any stream applies) — it attests both peers
        /// agree on the world's generation inputs, not that the standing scene is a valid reconstruction,
        /// so the caller guarantees the scene's derived content is valid for @p key. The scene is
        /// borrowed and must outlive the join. Two joins over one scene (a swap) is supported — their
        /// wire-id spaces are disjoint.
        /// @param key         The opaque world to join.
        /// @param adoptScene  The live scene the join binds to and streams into (borrowed).
        /// @param payload     The opaque travel payload threaded into the server's resolution.
        void JoinInto(const Net::WorldKey& key, Scene& adoptScene,
                      const Net::TravelPayload& payload = {});

        /// @brief Requests a server-directed travel to a world by key, carrying an opaque payload.
        ///
        /// Sends a travel request the server resolves and answers with a directed travel (join this
        /// world, and once ready leave the current one). Unlike Join, a travel lets the server resolve a
        /// payload-parameterized key and orchestrate the make-before-break — the client never
        /// self-resolves such a key. The reply's join runs the ordinary join flow (digest, payload).
        /// @param key      The opaque world to travel to.
        /// @param payload  The opaque travel payload the server resolves the key with; empty by default.
        void Travel(const Net::WorldKey& key, const Net::TravelPayload& payload = {});

        /// @brief Leaves a joined world without touching the scene beyond removing its footprint.
        ///
        /// Destroys exactly this join's wire-owned spawned set (recursively), releases its adopted
        /// anchor bindings (the claimants survive), demotes its predicted set, drops its
        /// ReplicationClient / prediction history / tick-sync, and sends the server a leave notice so it
        /// tears down the seat. The borrowed scene is left standing (a peer join may still present it);
        /// closing its runner world remains the caller's separate act. The make-before-break directed
        /// travel calls this internally once the destination is ready; a client may call it directly.
        /// @param join  The JoinId to leave; a no-op for an unknown join.
        void Leave(Net::JoinId join);

        /// @brief Pumps one frame: advance the connection, resolve join replies, apply each stream.
        /// @param now  Monotonic time in seconds (injected).
        void Pump(f64 now);

        /// @brief Registers the handler for a game message channel; one handler per channel.
        ///
        /// Inbound messages addressed to @p channel deliver to @p onMessage at DeliverMessages —
        /// the sender is always the server (the only peer), so the handler takes only the blob.
        /// Registering an already-registered channel replaces its handler. A message on a channel
        /// with no handler drops with a one-shot per-channel log.
        /// @param channel    The minted channel id to receive on.
        /// @param onMessage  The handler invoked per delivered message.
        void RegisterChannel(Net::ChannelId channel, function<void(const Net::Blob&)> onMessage);

        /// @brief Queues a game message to the server on a channel; fails loudly, never silently.
        ///
        /// Client → server is the only client direction (no client ↔ client; everything routes
        /// through the host) and doubles as the write lane into host-side services: a remote
        /// client's state-affecting request rides a channel the owning service validates and
        /// applies. Reliable-ordered within the live connection, at-most-once across its lifetime.
        /// Fails with the reason when the client is not connected, the payload exceeds
        /// Net::MaxMessagePayloadSize (no fragmentation), or the outbound queue is at its cap
        /// (Net::MaxOutboundMessages / Net::MaxOutboundMessageBytes, whichever first). Queued
        /// messages flush on the next Pump.
        /// @param channel  The channel the server's handler is registered on.
        /// @param payload  The opaque message blob; moved into the queue.
        /// @return Empty on acceptance, or the failure reason.
        VoidResult Send(Net::ChannelId channel, Net::Blob payload);

        /// @brief Delivers queued inbound game messages to their registered channel handlers.
        ///
        /// Receipt is frame-safe: Pump only queues inbound messages, and this dispatches them —
        /// call it at a point outside any scene iteration or sim tick (Application calls it at its
        /// top-of-frame request-drain slot). Messages deliver in arrival order. A message on an
        /// unregistered channel drops with a one-shot per-channel log.
        void DeliverMessages();

        /// @brief The JoinIds this client currently holds, in ascending order.
        /// @return The joined worlds' JoinIds, empty before the first join lands.
        [[nodiscard]] vector<Net::JoinId> Joins() const;

        /// @brief The JoinId of the current (first) joined world, or ControlJoinId before any lands.
        [[nodiscard]] Net::JoinId CurrentJoinId() const;

        /// @brief The current join's client scene, or nullptr before the first join loads it.
        [[nodiscard]] Scene* World() const;

        /// @brief A specific join's client scene, or nullptr for an unknown JoinId.
        /// @param join  The JoinId to resolve.
        [[nodiscard]] Scene* World(Net::JoinId join) const;

        /// @brief The current join's replication client (its NetId → Entity map, spawn arm).
        [[nodiscard]] ReplicationClient& Replication();

        /// @brief A specific join's replication client; asserts on an unknown JoinId.
        /// @param join  The JoinId to resolve.
        [[nodiscard]] ReplicationClient& Replication(Net::JoinId join);

        /// @brief The current join's own seat entity once it binds, or Entity::Null.
        [[nodiscard]] Entity Seat() const;

        /// @brief A specific join's own seat entity once it binds, or Entity::Null.
        /// @param join  The JoinId to resolve.
        [[nodiscard]] Entity Seat(Net::JoinId join) const;

        /// @brief The pawn the current join's own seat possesses (as last wired), or Entity::Null.
        [[nodiscard]] Entity PossessedPawn() const;

        /// @brief The travel payload the server echoed for a join, so the client's reconstruction has its inputs.
        ///
        /// The reply echoes the bucket's recorded params; a game reads them here to parameterize its
        /// procedural reconstruction of the joined world. Empty for an unknown join or a payload-free one.
        /// @param join  The JoinId to resolve.
        /// @return The echoed payload, or an empty payload.
        [[nodiscard]] const Net::TravelPayload& JoinPayload(Net::JoinId join) const;

        /// @brief The prediction history for the current join's predicted set.
        [[nodiscard]] Net::PredictionHistory& History();

        /// @brief The prediction history (read-only) for the current join's predicted set.
        [[nodiscard]] const Net::PredictionHistory& History() const;

        /// @brief The prediction history for a specific join; asserts on an unknown JoinId.
        /// @param join  The JoinId to resolve.
        [[nodiscard]] Net::PredictionHistory& History(Net::JoinId join);

        /// @brief Records the current join's predicted state and local input for a client tick.
        /// @param tick  The client sim tick whose predicted state and input are recorded.
        void RecordPrediction(u64 tick);

        /// @brief Records a specific join's predicted state and local input for a client tick.
        /// @param join  The JoinId whose predicted set is recorded.
        /// @param tick  The client sim tick whose predicted state and input are recorded.
        void RecordPrediction(Net::JoinId join, u64 tick);

        /// @brief Whether the current (first) join has loaded and acked its readiness.
        [[nodiscard]] bool IsJoined() const;

        /// @brief Whether a specific join has loaded and acked its readiness.
        /// @param join  The JoinId to test.
        [[nodiscard]] bool IsJoined(Net::JoinId join) const;

        /// @brief The highest server sim tick a snapshot has carried for the current join, or 0.
        [[nodiscard]] u64 LastServerTick() const;

        /// @brief The highest server sim tick a snapshot has carried for a specific join, or 0.
        /// @param join  The JoinId to resolve.
        [[nodiscard]] u64 LastServerTick(Net::JoinId join) const;

        /// @brief The current join's tick-offset controller — smoothed RTT/jitter and the running estimate.
        [[nodiscard]] const Net::TickOffsetEstimator& TickSync() const;

        /// @brief A specific join's tick-offset controller; asserts on an unknown JoinId.
        /// @param join  The JoinId to resolve.
        [[nodiscard]] const Net::TickOffsetEstimator& TickSync(Net::JoinId join) const;

        /// @brief Folds this frame's link state into the current join's tick-offset controller.
        /// @param clientTick  The client's current sim tick.
        /// @return The bounded step multiplier to apply next tick, or 1.0 when not yet syncing.
        f32 ObserveTickSync(u64 clientTick);

        /// @brief Folds this frame's link state into a specific join's tick-offset controller.
        /// @param join        The JoinId whose controller is updated.
        /// @param clientTick  The client's current sim tick.
        /// @return The bounded step multiplier, or 1.0 when not yet syncing.
        f32 ObserveTickSync(Net::JoinId join, u64 clientTick);

        /// @brief Sets the closed-loop feedback trim on the current join's tick-offset controller.
        /// @param trimTicks  The server's consumed-input early/late correction.
        void SetTickSyncFeedback(f32 trimTicks);

    private:
        struct State;

        explicit ClientHost(Unique<State> state);

        Unique<State> m_State;
    };
}
