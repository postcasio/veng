#pragma once

#include <Veng/Veng.h>
#include <Veng/Scene/Entity.h>

#include <span>

namespace Veng
{
    class Scene;

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
}
