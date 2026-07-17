#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Input/Actions.h>
#include <Veng/Net/Connection.h>
#include <Veng/Net/NetEvents.h>
#include <Veng/Net/Quantize.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Result.h>
#include <Veng/Scene/Entity.h>

#include <span>

namespace Veng
{
    class Scene;
    class Prefab;
    class AssetManager;
    class TypeRegistry;

    /// @brief The wire identity of a replicated entity — a server-assigned id the two ends agree on.
    ///
    /// Mirrors NetIdentity::Id. Zero is reserved: never assigned, and the null-reference value a
    /// replicated Entity field encodes to (a null target, or a target with no NetIdentity).
    using NetId = u32;

    /// @brief The reserved null wire id: unassigned, and the value a null/unreplicated reference encodes to.
    inline constexpr NetId InvalidNetId = 0;

    /// @brief Hands out fresh NetIds from a monotonic counter — the server's authority over wire identity.
    ///
    /// Ids are never reused: a destroyed entity's id is retired, so a stale wire reference can never
    /// silently resolve to a different entity. Starts at one, so zero stays the reserved null id.
    class NetIdAllocator
    {
    public:
        /// @brief Returns the next fresh id (monotonic, never reused, never zero).
        [[nodiscard]] NetId Next() { return ++m_Counter; }

        /// @brief Returns the most recently issued id (zero before the first Next()).
        [[nodiscard]] NetId Last() const { return m_Counter; }

    private:
        /// @brief The last issued id; ++ before use so the first id is one.
        NetId m_Counter = InvalidNetId;
    };

    /// @brief The displaying end's NetId → Entity map — net-layer-owned, rebuilt from spawned entities.
    ///
    /// A client resolves each snapshot record's NetId to a local Entity through this, and remaps every
    /// replicated Entity field the same way. It is owned by the net layer (not the Scene): the flow
    /// layer binds an id when it spawns the entity carrying that NetIdentity and unbinds it on
    /// despawn. A lookup miss is normal during join (an entity's snapshot can arrive before its spawn)
    /// and is the caller's to handle — the codec simply drops the record, idempotent against the next
    /// snapshot.
    class NetIdMap
    {
    public:
        /// @brief Binds (or rebinds) @p id to @p entity.
        void Bind(NetId id, Entity entity);

        /// @brief Unbinds @p id; a no-op if unbound.
        void Unbind(NetId id);

        /// @brief Returns the entity bound to @p id, or Entity::Null if unbound.
        [[nodiscard]] Entity Lookup(NetId id) const;

        /// @brief Drops every binding.
        void Clear();

        /// @brief Returns the number of bindings.
        [[nodiscard]] usize Size() const;

        /// @brief The wire-id → entity bindings, for a caller walking the whole spawned set.
        [[nodiscard]] const unordered_map<NetId, Entity>& Bindings() const { return m_Bindings; }

        /// @brief Rebuilds the map from every NetIdentity component in @p scene (bindings cleared first).
        ///
        /// The map is derived state: a client rebuilds it from the entities it has spawned rather than
        /// tracking every bind by hand. A const scene walk — it stamps no change ticks.
        void RebuildFrom(const Scene& scene);

    private:
        /// @brief The bindings; keyed by wire id.
        unordered_map<NetId, Entity> m_Bindings;
    };

    /// @brief Cross-join record of which live join currently binds each anchor's claimant.
    ///
    /// Stable-anchor adoption is **single-source**: a derived entity's authoritative state comes from
    /// exactly one join at a time. A ReplicationClient records its adoptions here; a second live join
    /// (a different ReplicationClient sharing this registry) binding an already-bound anchor is a fatal
    /// assert, the tripwire on the single-source invariant. A ClientHost owns one registry shared across
    /// its joins; a ReplicationClient with no shared registry uses its own private one (a lone client
    /// has no peer join to contend with). Keyed by the anchor's two opaque halves — the engine never
    /// interprets them.
    class AnchorBindings
    {
    public:
        /// @brief The JoinId currently binding the anchor (@p lo, @p hi), or ControlJoinId if unbound.
        [[nodiscard]] Net::JoinId OwnerOf(u64 lo, u64 hi) const;

        /// @brief Records that @p join binds the anchor (@p lo, @p hi)'s claimant.
        void Bind(u64 lo, u64 hi, Net::JoinId join);

        /// @brief Releases the anchor (@p lo, @p hi)'s binding; a no-op if unbound.
        void Release(u64 lo, u64 hi);

    private:
        /// @brief Anchor halves → the join that binds its claimant (std::map keys the 128-bit pair).
        map<std::pair<u64, u64>, Net::JoinId> m_Owners;
    };

    /// @brief Assigns a fresh NetIdentity to every server-authoritative entity in @p scene that lacks one.
    ///
    /// The server's identity pass, run at world load and after each spawn: an entity is
    /// server-authoritative when it has no Authority component (the default is Server) or its
    /// Authority tier is Server. An entity that already carries a NetIdentity keeps its id (ids are
    /// stable for an entity's lifetime). Local-tier entities (client-derived view entities) get none —
    /// they never cross the wire.
    /// @param scene      The scene to assign identities in.
    /// @param allocator  The id source; advanced once per newly-identified entity.
    /// @return The number of entities newly assigned an id.
    VE_API usize AssignServerNetIds(Scene& scene, NetIdAllocator& allocator);

    /// @brief One predicted entity's authoritative component state decoded from a snapshot.
    ///
    /// A Tier::Predicted entity is simulated locally, so its authoritative snapshot state is not
    /// applied to the live pose the way a Remote mirror's Transform is buffered — it is handed to the
    /// reconciler, which compares it against the recorded prediction and restores/replays on a
    /// mismatch. ReplicationClient::ApplySnapshot decodes these (references remapped to local handles)
    /// for every predicted entity in the packet and exposes them for that pump's reconciliation.
    struct PredictedRecord
    {
        /// @brief One authoritative component's reflected type and its local-form WriteFields bytes.
        struct Component
        {
            /// @brief The component's reflected type.
            TypeId Type = InvalidTypeId;
            /// @brief The authoritative state as WriteFields bytes, references already remapped to local handles.
            vector<u8> Bytes;
        };

        /// @brief The local entity the record resolved to.
        Entity Entity = Entity::Null;
        /// @brief The authoritative replicated components the snapshot carried for the entity.
        vector<Component> Components;
    };

    /// @brief The outcome of applying a snapshot packet, for the caller and for tests.
    struct SnapshotApplyResult
    {
        /// @brief The packet header's server tick, or 0 when the header was truncated.
        u64 ServerTick = 0;
        /// @brief The header's per-connection input feedback signal (see EncodeSnapshot), or 0.
        i32 InputFeedback = 0;
        /// @brief The client tick whose input the server had consumed when it simulated this snapshot, or 0.
        ///
        /// The confirmation signal reconciliation keys on: a snapshot at server tick ServerTick with
        /// LastConsumedInputTick == C means "authoritative state for the predicted set as of my input
        /// C". The client compares its recorded prediction at C against this snapshot's authoritative
        /// record and, on a mismatch, restores to C and replays. Zero before the server has consumed
        /// any of this connection's input.
        u64 LastConsumedInputTick = 0;
        /// @brief True when the header parsed (the packet carried a full header).
        bool HeaderValid = false;
        /// @brief Entity records whose NetId resolved and whose state was applied.
        u32 EntitiesApplied = 0;
        /// @brief Entity records dropped because their NetId was unbound in the map.
        u32 EntitiesDropped = 0;
    };

    /// @brief Serialized size of a snapshot packet header: the server tick, input feedback, and last-consumed input tick.
    inline constexpr usize SnapshotHeaderSize = sizeof(u64) + sizeof(i32) + sizeof(u64);

    /// @brief Encodes a snapshot of the scene's dirty replicated state into a self-delimiting packet.
    ///
    /// Walks every entity carrying a NetIdentity and, for each Replicated component whose change tick
    /// exceeds @p sinceTick, emits a per-component record. An entity with no dirty replicated component
    /// is omitted. Each entity record is independently applicable, so the caller packs records into
    /// MTU-sized packets greedily and a snapshot larger than one packet is simply several packets — no
    /// fragmentation. A replicated Entity field is written as its target's NetId (or the null id when
    /// the target is null or unreplicated), the prefab-remap discipline applied to the wire.
    ///
    /// Packet layout (framing little-endian; component payloads are the reflection serializer's
    /// WriteFields bytes):
    ///
    ///     SnapshotPacket  := ServerTick:u64  InputFeedback:i32  LastConsumedInputTick:u64  EntityRecord*
    ///     EntityRecord    := NetId:u32  ComponentCount:u32  ComponentRecord*
    ///     ComponentRecord := TypeId:u64  ByteLength:u32  WriteFields-bytes
    ///
    /// A const scene walk — it stamps no change ticks on the components it reads.
    /// @param scene                  The server scene to snapshot.
    /// @param serverTick             The tick this snapshot represents (the packet header).
    /// @param sinceTick              A component is included when its change tick exceeds this (the last-acked tick).
    /// @param inputFeedback          The per-connection input-timing feedback ridden in the header (see
    ///                               ReplicationServer::SetInputFeedback); zero for none.
    /// @param lastConsumedInputTick  The client tick whose input the server had consumed when it simulated
    ///                               this state (see ReplicationServer::SetLastConsumedInputTick); zero for none.
    /// @return The encoded packet bytes.
    [[nodiscard]] VE_API vector<u8> EncodeSnapshot(const Scene& scene, u64 serverTick,
                                                   u64 sinceTick, i32 inputFeedback = 0,
                                                   u64 lastConsumedInputTick = 0);

    /// @brief Applies a snapshot packet to @p scene, latest-wins and recoverable.
    ///
    /// Resolves each entity record's NetId through @p map and overwrites the resolved entity's
    /// components (adding an absent one), remapping replicated Entity fields back through the same map.
    /// Recovery is per the reflection serializer's posture: an unbound NetId drops the record (the next
    /// snapshot is idempotent), an unregistered TypeId skips that component (drift tolerance), a record
    /// that fails to decode leaves that component's prior state intact, and a truncated trailing record
    /// stops the walk without disturbing what already applied.
    /// @param packet  The encoded packet bytes.
    /// @param scene   The client scene to apply into.
    /// @param map     The NetId → Entity map resolving record ids and reference fields.
    /// @return A summary of what applied (see SnapshotApplyResult).
    VE_API SnapshotApplyResult ApplySnapshot(std::span<const u8> packet, Scene& scene,
                                             const NetIdMap& map);

    /// @brief Why an entity despawned on a connection — destruction versus a relevancy (interest) exit.
    ///
    /// A visibility despawn is not a destruction: the client tears the entity down locally with no
    /// game-event side effects (it must never read as death), and a re-entry re-spawns and
    /// re-baselines it. The one sharp edge of interest management, carried on the wire so game logic
    /// can tell the two apart.
    enum class DespawnReason : u8
    {
        /// @brief The entity was destroyed on the server — a real removal.
        Destroyed = 0,
        /// @brief The entity left the connection's interest set — a visibility exit, not a death.
        Visibility = 1,
    };

    /// @brief One replication message to send a connection: the channel it rides and its bytes.
    ///
    /// The replication layer produces these; the app Sends each on its channel of
    /// the connection. Keeping the transport out of the layer makes the whole flow drivable device-free
    /// over two in-process scenes, exactly the two-world test fixture.
    struct ReplicationMessage
    {
        /// @brief The delivery discipline this message rides: reliable for spawn/despawn, unreliable for snapshots.
        Net::Channel Channel = Net::Channel::UnreliableSequenced;
        /// @brief The encoded message bytes to Send on Channel.
        vector<u8> Bytes;
    };

    /// @brief The server end of state replication: per-connection spawn/despawn + dirty snapshots.
    ///
    /// Tracks, per connection, which replicated entities it has already spawned and the highest tick it
    /// has acked. Generate() diffs the server scene against that per-connection state and returns the
    /// messages to send this tick: a reliable Spawn for each newly-replicated entity (carrying its
    /// prefab AssetId when one is associated, else its full component state), a reliable Despawn for
    /// each entity that has gone, and — on a snapshot-interval tick — the dirty state as one or more
    /// MTU-sized unreliable snapshot packets. It owns no transport and no NetId allocation (the caller
    /// runs AssignServerNetIds); it is pure per-connection bookkeeping over the codec above.
    class VE_API ReplicationServer
    {
    public:
        /// @brief Server-side replication cadence and wire-compression knobs.
        struct Settings
        {
            /// @brief Emit a snapshot every this many ticks (2 ⇒ 30 Hz at a 60 Hz sim).
            u64 SnapshotInterval = 2;
            /// @brief Whether the snapshot wire quantizes Transform's spatial leaves (lossy, wire-only).
            ///
            /// Off by default: the raw codec is lossless (field deltas only), so a byte-exact
            /// consumer stays exact. A game opts in through GameNetInfo::QuantizeSpatial to trade the
            /// last decimal places of a displayed pose for bandwidth; the sim state stays full-float.
            bool QuantizeSpatial = false;
            /// @brief The spatial quantization applied when QuantizeSpatial is set.
            Net::QuantizationSettings Quantization;
            /// @brief Force a full (non-delta) record every this many snapshots to a connection.
            ///
            /// Bounds baseline drift and re-bases a connection that lost sync: the delta encoding is
            /// negotiated by ack, and a periodic keyframe is the self-heal a lossy link relies on.
            /// Zero disables keyframes (delta whenever a baseline is shared).
            u32 KeyframeInterval = 16;
        };

        /// @brief Constructs a replication server with the default cadence.
        ReplicationServer() = default;

        /// @brief Constructs a replication server with the given cadence.
        /// @param settings  The snapshot cadence.
        explicit ReplicationServer(const Settings& settings) : m_Settings(settings) {}

        /// @brief Registers a connection to replicate to, with a fresh (empty) baseline.
        ///
        /// Its first Generate streams every current replicated entity as a Spawn (the baseline spawn
        /// stream) and, thereafter, only the diffs. A no-op if the connection is already tracked.
        /// @param id  The connection to begin replicating to.
        void AddConnection(Net::ConnectionId id);

        /// @brief Stops tracking a connection and drops its per-connection state.
        /// @param id  The connection that has gone.
        void RemoveConnection(Net::ConnectionId id);

        /// @brief Associates a replicated entity's originating prefab, so its Spawn rides as an AssetId.
        ///
        /// Keyed by the entity's NetId (assigned by AssignServerNetIds). Without an association a
        /// Spawn carries the entity's full component state (the runtime-constructed arm). Setting the
        /// invalid AssetId clears the association.
        /// @param id      The replicated entity's NetId.
        /// @param prefab  The prefab the entity was spawned from, or the invalid id to clear.
        void SetEntityPrefab(NetId id, AssetId prefab);

        /// @brief Advances a connection's acked tick, gating which state its snapshots still carry.
        ///
        /// A component enters a connection's snapshot only while its change tick exceeds this — the
        /// send-until-acked rule. Acks arrive from the client (a piggybacked snapshot ack, or a standalone ack
        /// message); until one does, a connection's baseline stays at zero and every snapshot carries
        /// full state (idempotent, just more bandwidth).
        /// @param id    The connection acknowledging.
        /// @param tick  The highest server tick it has applied.
        void Acknowledge(Net::ConnectionId id, u64 tick);

        /// @brief Sets a connection's input-timing feedback, ridden in its next snapshot header.
        ///
        /// The signal the client's tick-offset controller reads to trim its lead: positive means the
        /// client's input is arriving earlier than needed (it can run less far ahead), negative that
        /// it is running late. A no-op for an untracked connection.
        /// @param id        The connection the feedback concerns.
        /// @param feedback  The feedback in ticks (see EncodeSnapshot's header field).
        void SetInputFeedback(Net::ConnectionId id, i32 feedback);

        /// @brief Sets the client tick whose input this connection's next snapshot reflects.
        ///
        /// The correctness-bearing sibling of SetInputFeedback, ridden in the connection's next
        /// snapshot header (see SnapshotApplyResult::LastConsumedInputTick): the client reconciles
        /// its recorded prediction at this tick against the authoritative snapshot. The server sets
        /// it from the tick it scheduled-consumed this connection's input for. A no-op for an
        /// untracked connection.
        /// @param id    The connection the confirmation concerns.
        /// @param tick  The client input tick the server had consumed when it simulated the snapshot.
        void SetLastConsumedInputTick(Net::ConnectionId id, u64 tick);

        /// @brief Diffs the scene against a connection's state, returning this tick's messages to send.
        ///
        /// Emits a reliable Spawn for each replicated entity new to the connection, a reliable Despawn
        /// for each it had that is now gone, and — when @p tick is a snapshot-interval tick — the dirty
        /// state (since the connection's acked tick) as MTU-sized unreliable snapshot packets. Updates
        /// the connection's spawned set. A no-op returning empty for an untracked connection.
        /// @param id     The connection to generate for (must be AddConnection'd).
        /// @param scene  The authoritative server scene.
        /// @param tick   The current server tick.
        /// @param interest  The connection's interest set gating which entities it hears about; null
        ///                  means every live entity is relevant (interest off — planset-54 behavior).
        ///                  An entity that was spawned but has left the set gets a visibility despawn.
        /// @return The messages to Send on this connection, each tagged with its channel.
        [[nodiscard]] vector<ReplicationMessage> Generate(Net::ConnectionId id, const Scene& scene,
                                                          u64 tick,
                                                          const set<NetId>* interest = nullptr);

    private:
        /// @brief One snapshot's sent component values, kept until acked so the baseline can advance.
        ///
        /// The delta encoder deltas against the value the connection has acknowledged; recovering
        /// that value on an ack needs the states sent in the unacked window, not the whole history.
        /// Bounded by the window (dropped once acked), never a growing ring.
        struct SentSnapshot
        {
            /// @brief The server tick this snapshot was sent on.
            u64 Tick = 0;
            /// @brief The component wire bytes sent this snapshot, keyed by NetId then TypeId.
            unordered_map<NetId, unordered_map<TypeId, vector<u8>>> Components;
        };

        /// @brief Per-connection replication bookkeeping.
        struct ConnectionState
        {
            /// @brief NetIds already spawned to this connection.
            set<NetId> Spawned;
            /// @brief Highest tick this connection has acked; snapshots gate against it.
            u64 AckedTick = 0;
            /// @brief Input-timing feedback ridden in this connection's next snapshot header.
            i32 InputFeedback = 0;
            /// @brief Client input tick the server had consumed, ridden in the next snapshot header.
            u64 LastConsumedInputTick = 0;
            /// @brief The acked baseline the delta encoder deltas against (component wire bytes).
            unordered_map<NetId, unordered_map<TypeId, vector<u8>>> Baseline;
            /// @brief The tick the Baseline currently represents (advanced as acks arrive).
            u64 AdoptedTick = 0;
            /// @brief The unacked sent snapshots, ascending by tick, adopted into Baseline on ack.
            vector<SentSnapshot> InFlight;
            /// @brief Snapshots generated for this connection, driving the keyframe cadence.
            u64 SnapshotCounter = 0;
            /// @brief The world tick this connection's last snapshot was emitted on.
            ///
            /// The snapshot cadence gates on the world's own sim tick, but Generate runs once per
            /// host pump — faster than a sub-rate world advances — so the same qualifying tick would
            /// otherwise emit a fresh snapshot on every pump it persists. Recording the tick a
            /// snapshot was sent on, and skipping a repeat of it, collapses that to one snapshot per
            /// qualifying world tick regardless of pump rate. The sentinel (never a real tick) fires
            /// the first snapshot, including one at tick 0.
            u64 LastSnapshotTick = ~u64{0};
        };

        Settings m_Settings;
        unordered_map<Net::ConnectionId, ConnectionState> m_Connections;
        unordered_map<NetId, AssetId> m_EntityPrefabs;
    };

    /// @brief The client end of state replication: applies spawn/despawn and latest-wins snapshots.
    ///
    /// Owns the client's NetId → Entity map. A reliable Spawn instantiates the entity — through the
    /// ordinary prefab path when the message names a prefab, or as a bare entity with its full
    /// component state otherwise — then stamps NetIdentity, binds the map, and marks it
    /// Authority{ Tier::Remote }. A reliable Despawn destroys it. An unreliable snapshot applies
    /// latest-wins: non-spatial replicated state writes straight onto the components, while each
    /// Transform record appends a tick-keyed sample to the entity's RemoteInterpolation buffer (the
    /// View-phase RemoteInterpolationSystem renders it in the past) rather than snapping the live pose.
    /// An unknown NetId drops its record — the next snapshot is idempotent.
    class VE_API ReplicationClient
    {
    public:
        /// @brief The outcome of applying one reliable message, for the caller and for tests.
        struct ReliableApplyResult
        {
            /// @brief True when the message was a Spawn that instantiated or adopted an entity.
            bool Spawned = false;
            /// @brief True when a Spawn bound its wire id to a live local claimant rather than a fresh entity.
            ///
            /// An anchored spawn whose anchor resolved to a claimant adopts it: no entity is created, the
            /// record's components apply onto the claimant, and its despawn releases (never destroys) it.
            bool Adopted = false;
            /// @brief True when the message was a Despawn that destroyed or released an entity.
            bool Despawned = false;
            /// @brief The despawn's reason (destruction vs a visibility exit); meaningful when Despawned.
            DespawnReason Reason = DespawnReason::Destroyed;
            /// @brief The NetId the message concerned (0 when the message was malformed).
            NetId Id = InvalidNetId;
            /// @brief The local entity spawned or despawned (null when none / already gone).
            Entity Entity = Entity::Null;
        };

        /// @brief Constructs a replication client resolving prefab spawns through @p resolvePrefab.
        /// @param resolvePrefab  Maps a prefab AssetId to a resident Prefab (the app's LoadSync; a
        ///                       null return falls the spawn back to the component-state arm).
        explicit ReplicationClient(function<Ref<Prefab>(AssetId)> resolvePrefab);

        /// @brief Enables stable-anchor adoption for this client, scoped to @p join over shared @p bindings.
        ///
        /// After this call an anchored spawn (a spawn carrying a NetAnchor) resolves a live local
        /// claimant in the target scene and adopts it instead of spawning a duplicate; the shared
        /// registry enforces single-source across a ClientHost's joins (@p join names this client's
        /// join). Unset (the default), the client still adopts but through a private registry — correct
        /// for a lone client with no peer join.
        /// @param join      The JoinId this client's adoptions are attributed to.
        /// @param bindings  The cross-join single-source registry (borrowed; must outlive the client).
        void SetAdoption(Net::JoinId join, AnchorBindings& bindings);

        /// @brief Returns the NetId → Entity map, rebuilt as spawns and despawns arrive.
        [[nodiscard]] const NetIdMap& Map() const { return m_Map; }

        /// @brief Sets the spatial quantization the snapshot decoder dequantizes with.
        ///
        /// Must match the server's ReplicationServer::Settings::Quantization; a mismatch dequantizes
        /// the wrong grid. The host threads the shared GameNetInfo value onto both ends.
        /// @param quant  The quantization settings.
        void SetQuantization(const Net::QuantizationSettings& quant) { m_Quantization = quant; }

        /// @brief Applies one reliable Spawn or Despawn message into @p scene.
        /// @param message  The reliable message bytes (a leading type byte + payload).
        /// @param scene    The client scene to spawn into or despawn from.
        /// @param assets   The asset manager the prefab-arm spawn resolves through.
        /// @return What the message did (see ReliableApplyResult).
        ReliableApplyResult ApplyReliable(std::span<const u8> message, Scene& scene,
                                          AssetManager& assets);

        /// @brief Applies one unreliable snapshot packet latest-wins into @p scene.
        ///
        /// Non-Transform components write straight onto the resolved entity; each Transform record
        /// appends a sample (keyed by the packet's server tick) to the entity's RemoteInterpolation
        /// buffer. An unbound NetId drops its record.
        /// @param packet  The snapshot packet bytes.
        /// @param scene   The client scene to apply into.
        /// @return A summary of what applied (see SnapshotApplyResult).
        SnapshotApplyResult ApplySnapshot(std::span<const u8> packet, Scene& scene);

        /// @brief The predicted entities' authoritative records decoded by the last ApplySnapshot.
        ///
        /// A Tier::Predicted entity's snapshot state is collected here rather than applied to the live
        /// pose (which is client-simulated); the reconciler consumes it against the header's
        /// LastConsumedInputTick. Cleared and refilled by each ApplySnapshot, so it is read
        /// immediately after that call, for that snapshot only.
        /// @return A view valid until the next ApplySnapshot.
        [[nodiscard]] std::span<const PredictedRecord> PredictedRecords() const
        {
            return m_PredictedRecords;
        }

        /// @brief Tears down this client's footprint in @p scene: destroy wire-owned, release adopted.
        ///
        /// The scene-preserving leave: each wire-owned entity this client spawned is destroyed
        /// (recursively, so replicated attachments go with their roots); each adopted claimant is
        /// released — its stream-added component types removed, its pre-existing values kept, the entity
        /// never destroyed and its anchor freed for the next join. Clears the id map, baselines, and
        /// adoption bookkeeping. The scene itself (a runner world) is the caller's.
        /// @param scene  The client scene this client applied its stream into.
        void Leave(Scene& scene);

    private:
        /// @brief Releases an adopted claimant: removes the stream-added types, frees the anchor binding.
        ///
        /// The claimant is never destroyed and its pre-existing components keep their last values. A
        /// no-op if @p id was not adopted. Drops the id map binding and delta baseline for @p id.
        /// @param scene  The scene the claimant lives in.
        /// @param id     The adopted wire id to release.
        void ReleaseAdopted(Scene& scene, NetId id);

        /// @brief One adopted claimant: the derived entity, its anchor, and the types the stream added.
        struct AdoptedEntity
        {
            /// @brief The live local entity the wire id was bound to.
            Entity Claimant = Entity::Null;
            /// @brief The anchor's low half (for releasing the shared binding).
            u64 AnchorLo = 0;
            /// @brief The anchor's high half.
            u64 AnchorHi = 0;
            /// @brief The component types the stream added at bind (absent before) — removed on release.
            vector<TypeId> AddedTypes;
        };

        NetIdMap m_Map;
        function<Ref<Prefab>(AssetId)> m_ResolvePrefab;
        /// @brief The last ApplySnapshot's Tier::Predicted authoritative records (see PredictedRecords).
        vector<PredictedRecord> m_PredictedRecords;
        /// @brief The per-(NetId, TypeId) baseline the delta decoder patches against (wire bytes).
        unordered_map<NetId, unordered_map<TypeId, vector<u8>>> m_Baseline;
        /// @brief The spatial quantization the decoder dequantizes with (see SetQuantization).
        Net::QuantizationSettings m_Quantization;
        /// @brief The JoinId this client's adoptions are attributed to in the shared registry.
        Net::JoinId m_Join = Net::ControlJoinId;
        /// @brief The single-source registry (shared when SetAdoption was called, else this private one).
        AnchorBindings m_OwnAnchors;
        AnchorBindings* m_Anchors = &m_OwnAnchors;
        /// @brief The adopted claimants, keyed by wire id, for snapshot routing and release.
        unordered_map<NetId, AdoptedEntity> m_Adopted;
        /// @brief Whether a claimant-less anchored spawn has already logged its one-shot warning.
        bool m_WarnedClaimantMiss = false;
    };

    // ---- Input replication (client → server) ------------------------------------------------------
    //
    // The other direction of the flow: a client stamps its seat's resolved PlayerInput per sim tick
    // and sends the last N ticks redundantly over the unreliable channel; the server buffers them per
    // connection in a jitter buffer and feeds the seat's PlayerInput at the matching tick, from which
    // the control system re-derives Intent unchanged. The ActionState is encoded through the reflection
    // serializer (its name-keyed FieldClass::Array form) — the v1 input wire format, no bespoke codec.

    /// @brief One seat input sample keyed by the client sim tick it was resolved on.
    struct TickedInput
    {
        /// @brief The client sim tick this input was stamped on.
        u64 ClientTick = 0;
        /// @brief The resolved action state for that tick (the PlayerInput wire payload).
        ActionState State;
    };

    /// @brief A decoded input packet: the piggybacked snapshot ack plus the client-tick-keyed input run.
    struct InputPacket
    {
        /// @brief The highest server snapshot tick the sender has applied; feeds ReplicationServer::Acknowledge.
        u64 AckedServerTick = 0;
        /// @brief The decoded inputs in ascending client-tick order; a per-record decode failure drops that record.
        vector<TickedInput> Inputs;
    };

    /// @brief Decays a duplicated input's edge phases so a coasted (underrun) tick repeats no edges.
    ///
    /// Started decays to Ongoing and Completed to None; Ongoing and None are unchanged. So a held
    /// button stays held across a duplicated tick while a trigger never re-fires — the edge semantics
    /// the jitter buffer preserves when it duplicates the last input on underrun.
    /// @param state  The input state to decay a copy of.
    /// @return The state with its edge phases decayed.
    [[nodiscard]] VE_API ActionState DecayInputPhases(const ActionState& state);

    /// @brief Encodes an input packet: the piggybacked ack + a redundant run of recent input ticks.
    ///
    /// The records cover the contiguous client ticks [firstClientTick, firstClientTick + records.size()),
    /// each the reflection encoding (WriteFields) of that tick's ActionState. Sending the last N ticks
    /// every packet makes the stream loss-tolerant without retransmission: a lost packet's ticks ride
    /// the next packet's overlap. An empty run encodes a header-only packet, so an input-idle client
    /// still carries its ack.
    ///
    /// Packet layout (framing little-endian; each record payload is the WriteFields bytes):
    ///
    ///     InputPacket := AckedServerTick:u64  FirstClientTick:u64  Count:u32  Record*
    ///     Record      := ByteLength:u32  WriteFields(ActionState)
    ///
    /// @param ackedServerTick  The highest server snapshot tick to acknowledge.
    /// @param firstClientTick  The client tick of the first record.
    /// @param records          The input states for the contiguous tick run, oldest first.
    /// @param registry         The type registry the ActionState encodes through.
    /// @return The encoded packet bytes.
    [[nodiscard]] VE_API vector<u8> EncodeInputPacket(u64 ackedServerTick, u64 firstClientTick,
                                                      std::span<const ActionState> records,
                                                      const TypeRegistry& registry);

    // ---- Packed input encoding (the context-keyed wire form) --------------------------------------
    //
    // The reflection form above is self-describing and drift-tolerant — the canonical, always-correct
    // input wire. The packed form is the opt-down: it encodes each action's value and phase against the
    // seat's active context's resolved action list (indices, value bits by action shape, 2-bit phase),
    // keyed by a context-stack hash both ends compute. The receiver decodes packed only when the
    // packet's hash matches its own resolved list; a mismatch (a mid-flight context switch) is the
    // documented fallback to the reflection form — the same negotiated-encoding posture as the deltas.

    /// @brief One action in a seat's resolved schema: its id and value shape, in stack-declared order.
    struct PackedInputAction
    {
        /// @brief The action's stable id (the schema is ordered; the wire encodes by position).
        ActionId Id = ActionId::Null;
        /// @brief The action's value shape, selecting how many value bits the wire carries.
        ActionKind Kind = ActionKind::Button;
    };

    /// @brief Hashes an ordered context-stack (its cooked context AssetIds) into a negotiation key.
    ///
    /// Order-sensitive: a different stack order yields a different hash, so both ends agree on the
    /// resolved action list only when the hash matches. The packed packet carries this; a mismatch
    /// falls the receiver back to the reflection form for that packet.
    /// @param contexts  The active context AssetIds, lowest priority first.
    /// @return The 64-bit stack hash.
    [[nodiscard]] VE_API u64 HashContextStack(std::span<const AssetId> contexts);

    /// @brief Encodes one action state against a resolved schema into a compact bit-packed record.
    ///
    /// Per schema action, in order: a presence bit, then (when present) a 2-bit phase and the value
    /// bits by shape — a button is one bit, a 1D axis eight quantized bits, a 2D axis two of those. An
    /// action in @p state absent from @p schema is dropped; a schema action absent from @p state is
    /// encoded absent.
    /// @param state   The resolved action state to encode.
    /// @param schema  The seat's ordered resolved action list.
    /// @return The bit-packed record bytes.
    [[nodiscard]] VE_API vector<u8>
    EncodePackedActionState(const ActionState& state, std::span<const PackedInputAction> schema);

    /// @brief Decodes a packed action-state record written by EncodePackedActionState.
    /// @param bytes   The bit-packed record.
    /// @param schema  The seat's ordered resolved action list (must match the encoder's).
    /// @return The reconstructed action state (axis values dequantized).
    [[nodiscard]] VE_API ActionState
    DecodePackedActionState(std::span<const u8> bytes, std::span<const PackedInputAction> schema);

    /// @brief Encodes an input packet in the packed form, carrying the context-stack hash.
    ///
    /// The packed sibling of EncodeInputPacket: the same ack + redundant tick run, each record the
    /// bit-packed form against @p schema, prefixed by @p contextHash so the receiver can verify its
    /// own resolved list matches before decoding.
    ///
    /// Packet layout (framing little-endian):
    ///
    ///     PackedInputPacket := AckedServerTick:u64  ContextHash:u64  FirstClientTick:u64  Count:u32  Record*
    ///     Record            := ByteLength:u32  EncodePackedActionState-bytes
    ///
    /// @param ackedServerTick  The highest server snapshot tick to acknowledge.
    /// @param contextHash      The sender's context-stack hash (see HashContextStack).
    /// @param firstClientTick  The client tick of the first record.
    /// @param records          The action states for the contiguous tick run, oldest first.
    /// @param schema           The seat's ordered resolved action list.
    /// @return The encoded packet bytes.
    [[nodiscard]] VE_API vector<u8>
    EncodePackedInputPacket(u64 ackedServerTick, u64 contextHash, u64 firstClientTick,
                            std::span<const ActionState> records,
                            std::span<const PackedInputAction> schema);

    /// @brief Decodes a packed input packet, verifying the context hash before decoding records.
    ///
    /// When the packet's carried hash does not equal @p expectedHash the packet is undecodable under
    /// this schema — a mid-flight context switch — and an error is returned so the caller falls back
    /// to the reflection form / drops the packet (input redundancy heals it). A truncated trailing
    /// record stops the walk; a header too short is the one hard error.
    /// @param packet        The encoded packet bytes.
    /// @param expectedHash  The receiver's own context-stack hash to verify against.
    /// @param schema        The receiver's ordered resolved action list.
    /// @return The decoded packet, or an error on a header truncation or a hash mismatch.
    [[nodiscard]] VE_API Result<InputPacket>
    DecodePackedInputPacket(std::span<const u8> packet, u64 expectedHash,
                            std::span<const PackedInputAction> schema);

    /// @brief Decodes an input packet, recoverable on malformed input.
    ///
    /// Reads the header, then each record by its length prefix. A record whose ActionState fails to
    /// decode is dropped (its tick skipped) while the surrounding records still apply; a truncated
    /// trailing record stops the walk. A packet too short to carry the header is the one hard failure —
    /// so a hostile packet drops input rather than asserting the server.
    /// @param packet    The encoded packet bytes.
    /// @param registry  The type registry the ActionState decodes through.
    /// @return The decoded packet, or an error string when the header is truncated.
    [[nodiscard]] VE_API Result<InputPacket> DecodeInputPacket(std::span<const u8> packet,
                                                               const TypeRegistry& registry);

    /// @brief The client's per-seat input send window: stamps each sim tick and encodes the last N redundantly.
    ///
    /// Each client sim tick, after InputMappingSystem resolves the local seat, Stamp records that tick's
    /// resolved input; the buffer retains only the last Redundancy ticks. Encode packs the retained
    /// window into one unreliable packet carrying the piggybacked snapshot ack — the loss-tolerant,
    /// no-retransmission input-redundancy scheme. It owns no transport; the app sends the bytes on the
    /// connection's unreliable channel.
    class VE_API InputSendBuffer
    {
    public:
        /// @brief Send-window sizing.
        struct Settings
        {
            /// @brief How many recent ticks each packet carries redundantly (the loss window).
            u32 Redundancy = 3;
        };

        /// @brief Constructs a send buffer with the default redundancy.
        InputSendBuffer() = default;

        /// @brief Constructs a send buffer with the given redundancy.
        /// @param settings  The send-window sizing.
        explicit InputSendBuffer(const Settings& settings) : m_Settings(settings) {}

        /// @brief Records this tick's resolved input, evicting the oldest beyond the redundancy window.
        /// @param clientTick  The client sim tick being stamped (monotonic, +1 per tick).
        /// @param state       The seat's resolved ActionState for this tick.
        void Stamp(u64 clientTick, const ActionState& state);

        /// @brief Encodes the retained window into a packet acknowledging @p ackedServerTick.
        ///
        /// Header-only (no records) before the first Stamp, so the ack still flows on an input-idle tick.
        /// @param ackedServerTick  The highest server snapshot tick to acknowledge.
        /// @param registry         The type registry the ActionState encodes through.
        /// @return The encoded packet bytes to send on the unreliable channel.
        [[nodiscard]] vector<u8> Encode(u64 ackedServerTick, const TypeRegistry& registry) const;

        /// @brief The number of ticks currently retained (at most Redundancy).
        [[nodiscard]] usize Size() const { return m_Window.size(); }

    private:
        Settings m_Settings;
        vector<TickedInput> m_Window;
    };

    /// @brief The server's per-connection input jitter buffer: buffers client ticks and feeds one per server tick.
    ///
    /// Ingest keys each packet's inputs by client tick (redundant duplicates collapse for free, inputs
    /// at or before the last consumed tick drop). Consume, called once per server tick, returns the next
    /// input in client-tick order and slews toward TargetDepth to absorb jitter: it drops the oldest
    /// buffered ticks on overrun (bounding latency) and duplicates the last input with its edge phases
    /// decayed on underrun (a held action persists, an edge never repeats). The consumed ActionState is
    /// written into the connection's seat PlayerInput before the Sim systems run, and the control system
    /// re-derives Intent from it unchanged.
    class VE_API InputJitterBuffer
    {
    public:
        /// @brief Buffer-depth tuning.
        struct Settings
        {
            /// @brief Target buffered depth Consume slews toward — the jitter the buffer absorbs.
            u32 TargetDepth = 2;
        };

        /// @brief Constructs a jitter buffer with the default target depth.
        InputJitterBuffer() = default;

        /// @brief Constructs a jitter buffer with the given target depth.
        /// @param settings  The depth tuning.
        explicit InputJitterBuffer(const Settings& settings) : m_Settings(settings) {}

        /// @brief Buffers a decoded packet's inputs, collapsing duplicates and dropping already-consumed ticks.
        /// @param packet  The decoded input packet.
        void Ingest(const InputPacket& packet);

        /// @brief Consumes the next input for this server tick, slewing toward the target depth.
        ///
        /// Drops the oldest buffered ticks when the depth exceeds the target (overrun), then pops the
        /// oldest remaining tick; when nothing is buffered (underrun) it duplicates the last consumed
        /// input with edge phases decayed. nullopt only before any input has ever arrived.
        /// @return The input to feed the seat this tick, or nullopt before the first input arrives.
        [[nodiscard]] optional<ActionState> Consume();

        /// @brief Consumes the input scheduled for a specific server tick (the ahead-of-server model).
        ///
        /// Drops any buffered client tick older than @p tick (the server has advanced past it), then
        /// returns the input stamped at @p tick when it has arrived. When it has not (the client is
        /// not running far enough ahead, or the packet was lost), it underruns — duplicating the last
        /// consumed input with its edge phases decayed, exactly as Consume does. Future ticks stay
        /// buffered for their own server tick. nullopt only before any input has ever arrived.
        /// @param tick  The server tick whose matching client input is consumed.
        /// @return The input to feed the seat this tick, or nullopt before the first input arrives.
        [[nodiscard]] optional<ActionState> ConsumeForTick(u64 tick);

        /// @brief The number of client ticks currently buffered.
        [[nodiscard]] usize Depth() const { return m_Buffer.size(); }

        /// @brief The highest client tick consumed or dropped so far (0 before the first Consume).
        [[nodiscard]] u64 LastConsumedTick() const { return m_LastConsumedTick; }

        /// @brief Consumes that yielded an input to feed the seat (a non-nullopt Consume/ConsumeForTick
        /// return, whether a fresh buffered tick or a coasted underrun) since construction. Empty polls
        /// before the first input has ever arrived — the only nullopt returns — are not counted, so this
        /// tracks actual inputs consumed rather than idle calls.
        [[nodiscard]] u64 ConsumeCount() const { return m_ConsumeCount; }

        /// @brief Consume calls that underran — coasted on the last input rather than a fresh one.
        [[nodiscard]] u64 UnderrunCount() const { return m_UnderrunCount; }

    private:
        Settings m_Settings;
        map<u64, ActionState> m_Buffer;
        optional<ActionState> m_Last;
        u64 m_LastConsumedTick = 0;
        u64 m_ConsumeCount = 0;
        u64 m_UnderrunCount = 0;
        bool m_Started = false;
    };
}
