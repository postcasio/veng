#pragma once

#include <Veng/Veng.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Net/Connection.h>
#include <Veng/Net/NetEvents.h>
#include <Veng/Scene/Entity.h>

#include <span>

namespace Veng
{
    class Scene;
    class Prefab;
    class AssetManager;

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

        /// @brief Rebuilds the map from every NetIdentity component in @p scene (bindings cleared first).
        ///
        /// The map is derived state: a client rebuilds it from the entities it has spawned rather than
        /// tracking every bind by hand. A const scene walk — it stamps no change ticks.
        void RebuildFrom(const Scene& scene);

    private:
        /// @brief The bindings; keyed by wire id.
        unordered_map<NetId, Entity> m_Bindings;
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

    /// @brief The outcome of applying a snapshot packet, for the caller and for tests.
    struct SnapshotApplyResult
    {
        /// @brief The packet header's server tick, or 0 when the header was truncated.
        u64 ServerTick = 0;
        /// @brief True when the header parsed (the packet carried at least a server tick).
        bool HeaderValid = false;
        /// @brief Entity records whose NetId resolved and whose state was applied.
        u32 EntitiesApplied = 0;
        /// @brief Entity records dropped because their NetId was unbound in the map.
        u32 EntitiesDropped = 0;
    };

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
    ///     SnapshotPacket  := ServerTick:u64  EntityRecord*
    ///     EntityRecord    := NetId:u32  ComponentCount:u32  ComponentRecord*
    ///     ComponentRecord := TypeId:u64  ByteLength:u32  WriteFields-bytes
    ///
    /// A const scene walk — it stamps no change ticks on the components it reads.
    /// @param scene       The server scene to snapshot.
    /// @param serverTick  The tick this snapshot represents (the packet header).
    /// @param sinceTick   A component is included when its change tick exceeds this (the last-acked tick).
    /// @return The encoded packet bytes.
    [[nodiscard]] VE_API vector<u8> EncodeSnapshot(const Scene& scene, u64 serverTick,
                                                   u64 sinceTick);

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

    /// @brief One replication message to send a connection: the channel it rides and its bytes.
    ///
    /// The replication layer produces these; the app (Plan 07's wiring) Sends each on its channel of
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
        /// @brief Server-side replication cadence.
        struct Settings
        {
            /// @brief Emit a snapshot every this many ticks (2 ⇒ 30 Hz at a 60 Hz sim).
            u64 SnapshotInterval = 2;
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
        /// send-until-acked rule. Acks arrive from the client (Plan 05's piggyback, or a standalone ack
        /// message); until one does, a connection's baseline stays at zero and every snapshot carries
        /// full state (idempotent, just more bandwidth).
        /// @param id    The connection acknowledging.
        /// @param tick  The highest server tick it has applied.
        void Acknowledge(Net::ConnectionId id, u64 tick);

        /// @brief Diffs the scene against a connection's state, returning this tick's messages to send.
        ///
        /// Emits a reliable Spawn for each replicated entity new to the connection, a reliable Despawn
        /// for each it had that is now gone, and — when @p tick is a snapshot-interval tick — the dirty
        /// state (since the connection's acked tick) as MTU-sized unreliable snapshot packets. Updates
        /// the connection's spawned set. A no-op returning empty for an untracked connection.
        /// @param id     The connection to generate for (must be AddConnection'd).
        /// @param scene  The authoritative server scene.
        /// @param tick   The current server tick.
        /// @return The messages to Send on this connection, each tagged with its channel.
        [[nodiscard]] vector<ReplicationMessage> Generate(Net::ConnectionId id, const Scene& scene,
                                                          u64 tick);

    private:
        /// @brief Per-connection replication bookkeeping.
        struct ConnectionState
        {
            /// @brief NetIds already spawned to this connection.
            set<NetId> Spawned;
            /// @brief Highest tick this connection has acked; snapshots gate against it.
            u64 AckedTick = 0;
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
            /// @brief True when the message was a Spawn that instantiated an entity.
            bool Spawned = false;
            /// @brief True when the message was a Despawn that destroyed an entity.
            bool Despawned = false;
            /// @brief The NetId the message concerned (0 when the message was malformed).
            NetId Id = InvalidNetId;
            /// @brief The local entity spawned or despawned (null when none / already gone).
            Entity Entity = Entity::Null;
        };

        /// @brief Constructs a replication client resolving prefab spawns through @p resolvePrefab.
        /// @param resolvePrefab  Maps a prefab AssetId to a resident Prefab (the app's LoadSync; a
        ///                       null return falls the spawn back to the component-state arm).
        explicit ReplicationClient(function<Ref<Prefab>(AssetId)> resolvePrefab);

        /// @brief Returns the NetId → Entity map, rebuilt as spawns and despawns arrive.
        [[nodiscard]] const NetIdMap& Map() const { return m_Map; }

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

    private:
        NetIdMap m_Map;
        function<Ref<Prefab>(AssetId)> m_ResolvePrefab;
    };
}
