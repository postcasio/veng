#include <Veng/Net/Replication.h>

#include <Veng/Assert.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Prefab.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Result.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/RemoteInterpolationSystem.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneClone.h>

#include <algorithm>
#include <new>

namespace Veng
{
    namespace
    {
        // Framing is written field-by-field little-endian (never a memcpy of a padded struct); the
        // component payloads embedded between the framing are the reflection serializer's WriteFields
        // bytes, the v1 wire codec (host byte order, a same-build assumption the whole net layer makes).

        void AppendU8(vector<u8>& out, u8 value)
        {
            out.push_back(value);
        }

        void AppendU32(vector<u8>& out, u32 value)
        {
            for (u32 i = 0; i < 4; ++i)
            {
                out.push_back(static_cast<u8>(value >> (8 * i)));
            }
        }

        void AppendU64(vector<u8>& out, u64 value)
        {
            for (u32 i = 0; i < 8; ++i)
            {
                out.push_back(static_cast<u8>(value >> (8 * i)));
            }
        }

        Result<u8> ReadU8(std::span<const u8> in, usize& cursor)
        {
            if (cursor + sizeof(u8) > in.size())
            {
                return std::unexpected("replication: truncated u8");
            }
            const u8 value = in[cursor];
            cursor += sizeof(u8);
            return value;
        }

        Result<u32> ReadU32(std::span<const u8> in, usize& cursor)
        {
            if (cursor + sizeof(u32) > in.size())
            {
                return std::unexpected("snapshot: truncated u32");
            }
            u32 value = 0;
            for (u32 i = 0; i < 4; ++i)
            {
                value |= static_cast<u32>(in[cursor + i]) << (8 * i);
            }
            cursor += sizeof(u32);
            return value;
        }

        Result<u64> ReadU64(std::span<const u8> in, usize& cursor)
        {
            if (cursor + sizeof(u64) > in.size())
            {
                return std::unexpected("snapshot: truncated u64");
            }
            u64 value = 0;
            for (u32 i = 0; i < 8; ++i)
            {
                value |= static_cast<u64>(in[cursor + i]) << (8 * i);
            }
            cursor += sizeof(u64);
            return value;
        }

        /// @brief RAII scratch storage for one type-erased component value.
        ///
        /// Aligned storage default-constructed through the type's thunks, so the codec can decode or
        /// remap a value out of line — never touching the live component until the decode succeeds.
        struct ScratchComponent
        {
            const TypeInfo& Info;
            void* Ptr;

            explicit ScratchComponent(const TypeInfo& info)
                : Info(info), Ptr(::operator new(info.Size, std::align_val_t{info.Align}))
            {
                Info.DefaultConstruct(Ptr);
            }

            ~ScratchComponent()
            {
                Info.Destruct(Ptr);
                ::operator delete(Ptr, std::align_val_t{Info.Align});
            }

            ScratchComponent(const ScratchComponent&) = delete;
            ScratchComponent& operator=(const ScratchComponent&) = delete;
        };

        // Replicated component TypeIds in a deterministic (sorted) order, so a snapshot's per-entity
        // component order is stable across runs.
        vector<TypeId> ReplicatedTypeIds(const TypeRegistry& registry)
        {
            vector<TypeId> ids;
            for (const auto& [id, info] : registry.All())
            {
                if (info.Replicated)
                {
                    ids.push_back(id);
                }
            }
            std::ranges::sort(ids);
            return ids;
        }

        // Encode-side remap: a replicated Entity field is written as its target's NetId (the reserved
        // null id for a null or unreplicated target — referencing a Local-tier entity from replicated
        // state is an authoring error). The NetId rides in the Entity's Index; the generation is unused.
        EntityRemap MakeEncodeRef(const Scene& scene)
        {
            const TypeId netIdentityId = TypeIdOf<NetIdentity>();
            return [&scene, netIdentityId](Entity local) -> Entity
            {
                if (local.IsNull() || !scene.IsAlive(local))
                {
                    return Entity{.Index = InvalidNetId, .Generation = 0};
                }
                const auto* identity =
                    static_cast<const NetIdentity*>(scene.TryGetComponent(local, netIdentityId));
                const NetId id = identity != nullptr ? identity->Id : InvalidNetId;
                return Entity{.Index = id, .Generation = 0};
            };
        }

        // Decode-side remap: a wire reference (its NetId in the Entity's Index) resolves back through
        // the id map; the reserved null id and an unbound id both resolve to Entity::Null.
        EntityRemap MakeDecodeRef(const NetIdMap& map)
        {
            return [&map](Entity wire) -> Entity
            {
                const NetId id = wire.Index;
                if (id == InvalidNetId)
                {
                    return Entity::Null;
                }
                return map.Lookup(id);
            };
        }

        // One entity's dirty replicated component records (each TypeId:u64 ByteLength:u32 bytes),
        // plus their count — the payload of a snapshot entity record and a spawn message.
        struct EncodedComponents
        {
            u32 Count = 0;
            vector<u8> Bytes;
        };

        EncodedComponents EncodeDirtyComponents(const Scene& scene, Entity entity,
                                                const vector<TypeId>& replicated, u64 sinceTick,
                                                const TypeRegistry& registry,
                                                const EntityRemap& encodeRef)
        {
            const AssetHandleFixup keepAsset = [](void*) {};
            EncodedComponents result;

            for (const TypeId typeId : replicated)
            {
                const void* component = scene.TryGetComponent(entity, typeId);
                if (component == nullptr)
                {
                    continue;
                }
                if (scene.GetComponentChangeTick(entity, typeId) <= sinceTick)
                {
                    continue;
                }

                const TypeInfo& info = registry.Info(typeId);

                // Copy the live value out of line, translate its Entity references to NetIds, then
                // serialize the copy — the live component is never mutated.
                vector<u8> valueCopy;
                WriteFields(valueCopy, component, info, registry);
                ScratchComponent scratch(info);
                ReadFields(valueCopy, scratch.Ptr, info, registry).value();
                RemapComponentReferences(scratch.Ptr, info, registry, encodeRef, keepAsset);

                vector<u8> payload;
                WriteFields(payload, scratch.Ptr, info, registry);

                AppendU64(result.Bytes, typeId);
                AppendU32(result.Bytes, static_cast<u32>(payload.size()));
                result.Bytes.insert(result.Bytes.end(), payload.begin(), payload.end());
                ++result.Count;
            }

            return result;
        }

        // Applies one entity record's component list (positioned at the first component record).
        // Decodes each component out of line so a malformed record leaves prior state intact. When
        // the record is for a known (bound, alive) entity, each component either buffers a Transform
        // sample (bufferTransform) or overwrites the live component; an unknown entity's records are
        // parsed only to keep the cursor aligned. Stops on a truncated record.
        void ApplyComponentRecords(std::span<const u8> packet, usize& cursor, u32 componentCount,
                                   Scene& scene, Entity entity, bool known,
                                   const TypeRegistry& registry, const EntityRemap& decodeRef,
                                   bool bufferTransform, u64 serverTick, bool& appliedOut,
                                   bool& truncatedOut)
        {
            const AssetHandleFixup keepAsset = [](void*) {};
            const TypeId transformId = TypeIdOf<Transform>();

            for (u32 i = 0; i < componentCount; ++i)
            {
                const Result<u64> typeId = ReadU64(packet, cursor);
                if (!typeId)
                {
                    truncatedOut = true;
                    return;
                }
                const Result<u32> byteLength = ReadU32(packet, cursor);
                if (!byteLength)
                {
                    truncatedOut = true;
                    return;
                }
                if (cursor + *byteLength > packet.size())
                {
                    truncatedOut = true;
                    return;
                }
                const std::span<const u8> payload = packet.subspan(cursor, *byteLength);
                cursor += *byteLength;

                if (!known)
                {
                    continue;
                }
                if (!registry.IsRegistered(*typeId))
                {
                    continue; // schema-drift tolerance
                }

                const TypeInfo& info = registry.Info(*typeId);

                ScratchComponent scratch(info);
                if (VoidResult read = ReadFields(payload, scratch.Ptr, info, registry); !read)
                {
                    continue; // malformed record: leave prior state intact
                }
                RemapComponentReferences(scratch.Ptr, info, registry, decodeRef, keepAsset);

                if (bufferTransform && *typeId == transformId)
                {
                    // A Transform snapshot feeds the interpolation buffer, not the live pose — the
                    // View-phase system renders it in the past.
                    const Transform& pose = *static_cast<const Transform*>(scratch.Ptr);
                    RemoteInterpolation& interp = scene.Has<RemoteInterpolation>(entity)
                                                      ? scene.Get<RemoteInterpolation>(entity)
                                                      : scene.Add<RemoteInterpolation>(entity);
                    // Unreliable-sequenced delivery gives monotone ticks; ignore a duplicate or older
                    // sample so the run stays ascending.
                    if (interp.Samples.empty() || serverTick > interp.Samples.back().ServerTick)
                    {
                        interp.Samples.push_back(RemoteSample{.ServerTick = serverTick,
                                                              .Position = pose.Position,
                                                              .Rotation = pose.Rotation,
                                                              .Scale = pose.Scale});
                    }
                    appliedOut = true;
                    continue;
                }

                void* dest = scene.TryGetComponent(entity, *typeId);
                if (dest == nullptr)
                {
                    dest = scene.AddComponent(entity, *typeId);
                }
                info.Destruct(dest);
                info.MoveConstruct(dest, scratch.Ptr);
                appliedOut = true;
            }
        }

        // The reliable message kinds the replication layer rides on the connection's reliable channel,
        // behind the same leading-type-byte frame the handshake uses. The values sit clear of the
        // ControlMessageType range (1–4) the lifecycle layer owns, so the Client/Server pump routes
        // these to the app as unrecognized-control app messages rather than treating them as handshake.
        enum class ReplicationMessageId : u8
        {
            Spawn = 16,
            Despawn = 17,
        };
    }

    void NetIdMap::Bind(NetId id, Entity entity)
    {
        m_Bindings[id] = entity;
    }

    void NetIdMap::Unbind(NetId id)
    {
        m_Bindings.erase(id);
    }

    Entity NetIdMap::Lookup(NetId id) const
    {
        const auto it = m_Bindings.find(id);
        return it != m_Bindings.end() ? it->second : Entity::Null;
    }

    void NetIdMap::Clear()
    {
        m_Bindings.clear();
    }

    usize NetIdMap::Size() const
    {
        return m_Bindings.size();
    }

    void NetIdMap::RebuildFrom(const Scene& scene)
    {
        m_Bindings.clear();
        for (auto [entity, identity] : scene.View<NetIdentity>())
        {
            if (identity.Id != InvalidNetId)
            {
                m_Bindings[identity.Id] = entity;
            }
        }
    }

    usize AssignServerNetIds(Scene& scene, NetIdAllocator& allocator)
    {
        const Scene& readScene = scene;
        const TypeId netIdentityId = TypeIdOf<NetIdentity>();
        const TypeId authorityId = TypeIdOf<Authority>();

        // Collect first: assigning adds a component (a structural change), illegal mid-walk.
        vector<Entity> targets;
        readScene.ForEachEntity(
            [&](Entity entity)
            {
                if (readScene.TryGetComponent(entity, netIdentityId) != nullptr)
                {
                    return;
                }
                const auto* authority =
                    static_cast<const Authority*>(readScene.TryGetComponent(entity, authorityId));
                const bool serverAuthoritative =
                    authority == nullptr || authority->Tier == Tier::Server;
                if (serverAuthoritative)
                {
                    targets.push_back(entity);
                }
            });

        for (const Entity entity : targets)
        {
            scene.Add<NetIdentity>(entity).Id = allocator.Next();
        }
        return targets.size();
    }

    vector<u8> EncodeSnapshot(const Scene& scene, u64 serverTick, u64 sinceTick)
    {
        const TypeRegistry& registry = scene.GetTypeRegistry();
        const vector<TypeId> replicated = ReplicatedTypeIds(registry);
        const EntityRemap encodeRef = MakeEncodeRef(scene);

        vector<u8> out;
        AppendU64(out, serverTick);

        for (auto [entity, identity] : scene.View<NetIdentity>())
        {
            const EncodedComponents encoded =
                EncodeDirtyComponents(scene, entity, replicated, sinceTick, registry, encodeRef);
            if (encoded.Count == 0)
            {
                continue;
            }
            AppendU32(out, identity.Id);
            AppendU32(out, encoded.Count);
            out.insert(out.end(), encoded.Bytes.begin(), encoded.Bytes.end());
        }

        return out;
    }

    SnapshotApplyResult ApplySnapshot(std::span<const u8> packet, Scene& scene, const NetIdMap& map)
    {
        const TypeRegistry& registry = scene.GetTypeRegistry();
        SnapshotApplyResult result;

        usize cursor = 0;
        const Result<u64> serverTick = ReadU64(packet, cursor);
        if (!serverTick)
        {
            // A packet too short to carry a header applies nothing.
            return result;
        }
        result.ServerTick = *serverTick;
        result.HeaderValid = true;

        const EntityRemap decodeRef = MakeDecodeRef(map);

        while (cursor < packet.size())
        {
            const Result<u32> netId = ReadU32(packet, cursor);
            if (!netId)
            {
                break;
            }
            const Result<u32> componentCount = ReadU32(packet, cursor);
            if (!componentCount)
            {
                break;
            }

            const Entity entity = map.Lookup(*netId);
            const bool known = !entity.IsNull() && scene.IsAlive(entity);

            bool applied = false;
            bool truncated = false;
            ApplyComponentRecords(packet, cursor, *componentCount, scene, entity, known, registry,
                                  decodeRef, /*bufferTransform=*/false, *serverTick, applied,
                                  truncated);

            if (!known)
            {
                ++result.EntitiesDropped;
            }
            else if (applied)
            {
                ++result.EntitiesApplied;
            }

            if (truncated)
            {
                break;
            }
        }

        return result;
    }

    void ReplicationServer::AddConnection(Net::ConnectionId id)
    {
        m_Connections.try_emplace(id);
    }

    void ReplicationServer::RemoveConnection(Net::ConnectionId id)
    {
        m_Connections.erase(id);
    }

    void ReplicationServer::SetEntityPrefab(NetId id, AssetId prefab)
    {
        if (prefab.IsValid())
        {
            m_EntityPrefabs[id] = prefab;
        }
        else
        {
            m_EntityPrefabs.erase(id);
        }
    }

    void ReplicationServer::Acknowledge(Net::ConnectionId id, u64 tick)
    {
        const auto it = m_Connections.find(id);
        if (it != m_Connections.end() && tick > it->second.AckedTick)
        {
            it->second.AckedTick = tick;
        }
    }

    vector<ReplicationMessage> ReplicationServer::Generate(Net::ConnectionId id, const Scene& scene,
                                                           u64 tick)
    {
        vector<ReplicationMessage> messages;

        const auto it = m_Connections.find(id);
        if (it == m_Connections.end())
        {
            return messages;
        }
        ConnectionState& state = it->second;

        const TypeRegistry& registry = scene.GetTypeRegistry();
        const vector<TypeId> replicated = ReplicatedTypeIds(registry);
        const EntityRemap encodeRef = MakeEncodeRef(scene);
        const TypeId authorityId = TypeIdOf<Authority>();

        // Diff the live replicated set against what this connection already has: a Spawn for each new
        // NetId, a Despawn for each vanished one. NetIdentity marks exactly the server-authoritative set.
        set<NetId> live;
        for (auto [entity, identity] : scene.View<NetIdentity>())
        {
            if (identity.Id == InvalidNetId)
            {
                continue;
            }
            live.insert(identity.Id);

            if (state.Spawned.contains(identity.Id))
            {
                continue;
            }
            state.Spawned.insert(identity.Id);

            const auto* authority =
                static_cast<const Authority*>(scene.TryGetComponent(entity, authorityId));
            const u32 owner = authority != nullptr ? authority->Owner : 0;

            const auto prefabIt = m_EntityPrefabs.find(identity.Id);
            const bool hasPrefab = prefabIt != m_EntityPrefabs.end();

            // The spawn carries the entity's full current replicated state (sinceTick 0), so the
            // client is whole without waiting a snapshot round.
            const EncodedComponents encoded =
                EncodeDirtyComponents(scene, entity, replicated, 0, registry, encodeRef);

            vector<u8> spawn;
            AppendU8(spawn, static_cast<u8>(ReplicationMessageId::Spawn));
            AppendU32(spawn, identity.Id);
            AppendU32(spawn, owner);
            AppendU8(spawn, hasPrefab ? 1 : 0);
            if (hasPrefab)
            {
                AppendU64(spawn, prefabIt->second.Value);
            }
            AppendU32(spawn, encoded.Count);
            spawn.insert(spawn.end(), encoded.Bytes.begin(), encoded.Bytes.end());
            messages.push_back(ReplicationMessage{.Channel = Net::Channel::ReliableOrdered,
                                                  .Bytes = std::move(spawn)});
        }

        // Despawns: NetIds this connection had that are no longer live.
        vector<NetId> gone;
        for (const NetId spawned : state.Spawned)
        {
            if (!live.contains(spawned))
            {
                gone.push_back(spawned);
            }
        }
        for (const NetId netId : gone)
        {
            state.Spawned.erase(netId);
            vector<u8> despawn;
            AppendU8(despawn, static_cast<u8>(ReplicationMessageId::Despawn));
            AppendU32(despawn, netId);
            messages.push_back(ReplicationMessage{.Channel = Net::Channel::ReliableOrdered,
                                                  .Bytes = std::move(despawn)});
        }

        // Snapshot on the interval tick: pack dirty entity records greedily into MTU-sized unreliable
        // packets, each a self-contained snapshot packet (header + a subset of the records).
        if (m_Settings.SnapshotInterval != 0 && tick % m_Settings.SnapshotInterval == 0)
        {
            const auto startPacket = [tick]()
            {
                vector<u8> packet;
                AppendU64(packet, tick);
                return packet;
            };

            vector<u8> current = startPacket();
            bool currentHasRecords = false;

            for (auto [entity, identity] : scene.View<NetIdentity>())
            {
                const EncodedComponents encoded = EncodeDirtyComponents(
                    scene, entity, replicated, state.AckedTick, registry, encodeRef);
                if (encoded.Count == 0)
                {
                    continue;
                }

                vector<u8> record;
                AppendU32(record, identity.Id);
                AppendU32(record, encoded.Count);
                record.insert(record.end(), encoded.Bytes.begin(), encoded.Bytes.end());

                if (currentHasRecords &&
                    current.size() + record.size() > Net::MaxUnreliableMessageSize)
                {
                    messages.push_back(ReplicationMessage{
                        .Channel = Net::Channel::UnreliableSequenced, .Bytes = std::move(current)});
                    current = startPacket();
                    currentHasRecords = false;
                }

                current.insert(current.end(), record.begin(), record.end());
                currentHasRecords = true;
            }

            if (currentHasRecords)
            {
                messages.push_back(ReplicationMessage{.Channel = Net::Channel::UnreliableSequenced,
                                                      .Bytes = std::move(current)});
            }
        }

        return messages;
    }

    ReplicationClient::ReplicationClient(function<Ref<Prefab>(AssetId)> resolvePrefab)
        : m_ResolvePrefab(std::move(resolvePrefab))
    {
    }

    ReplicationClient::ReliableApplyResult
    ReplicationClient::ApplyReliable(std::span<const u8> message, Scene& scene,
                                     AssetManager& assets)
    {
        ReliableApplyResult result;

        usize cursor = 0;
        const Result<u8> type = ReadU8(message, cursor);
        if (!type)
        {
            return result;
        }

        const TypeRegistry& registry = scene.GetTypeRegistry();

        switch (static_cast<ReplicationMessageId>(*type))
        {
        case ReplicationMessageId::Spawn:
        {
            const Result<u32> netId = ReadU32(message, cursor);
            const Result<u32> owner = ReadU32(message, cursor);
            const Result<u8> hasPrefab = ReadU8(message, cursor);
            if (!netId || !owner || !hasPrefab)
            {
                return result;
            }
            AssetId prefabId;
            if (*hasPrefab != 0)
            {
                const Result<u64> raw = ReadU64(message, cursor);
                if (!raw)
                {
                    return result;
                }
                prefabId = AssetId{.Value = *raw};
            }
            const Result<u32> componentCount = ReadU32(message, cursor);
            if (!componentCount)
            {
                return result;
            }

            // Instantiate through the ordinary prefab path when a resident prefab is named; otherwise
            // build a bare entity the component records fully construct.
            Entity entity = Entity::Null;
            if (prefabId.IsValid() && m_ResolvePrefab)
            {
                if (const Ref<Prefab> prefab = m_ResolvePrefab(prefabId))
                {
                    const Prefab::SpawnResult spawned = prefab->SpawnInto(scene, assets);
                    if (!spawned.Roots.empty())
                    {
                        entity = spawned.Roots.front();
                    }
                }
            }
            if (entity.IsNull())
            {
                entity = scene.CreateEntity();
            }

            const EntityRemap decodeRef = MakeDecodeRef(m_Map);
            bool applied = false;
            bool truncated = false;
            ApplyComponentRecords(message, cursor, *componentCount, scene, entity, /*known=*/true,
                                  registry, decodeRef, /*bufferTransform=*/false, /*serverTick=*/0,
                                  applied, truncated);
            (void)applied;
            (void)truncated;

            // Stamp the wire identity and mark the entity a remote mirror, then bind the map.
            if (scene.Has<NetIdentity>(entity))
            {
                scene.Get<NetIdentity>(entity).Id = *netId;
            }
            else
            {
                scene.Add<NetIdentity>(entity).Id = *netId;
            }
            if (scene.Has<Authority>(entity))
            {
                scene.Get<Authority>(entity) = Authority{.Tier = Tier::Remote, .Owner = *owner};
            }
            else
            {
                scene.Add<Authority>(entity, Authority{.Tier = Tier::Remote, .Owner = *owner});
            }
            m_Map.Bind(*netId, entity);

            result.Spawned = true;
            result.Id = *netId;
            result.Entity = entity;
            return result;
        }
        case ReplicationMessageId::Despawn:
        {
            const Result<u32> netId = ReadU32(message, cursor);
            if (!netId)
            {
                return result;
            }
            result.Id = *netId;
            const Entity entity = m_Map.Lookup(*netId);
            if (!entity.IsNull() && scene.IsAlive(entity))
            {
                scene.DestroyEntity(entity);
                result.Entity = entity;
            }
            m_Map.Unbind(*netId);
            result.Despawned = true;
            return result;
        }
        }

        return result; // unknown type — ignore
    }

    SnapshotApplyResult ReplicationClient::ApplySnapshot(std::span<const u8> packet, Scene& scene)
    {
        const TypeRegistry& registry = scene.GetTypeRegistry();
        SnapshotApplyResult result;

        usize cursor = 0;
        const Result<u64> serverTick = ReadU64(packet, cursor);
        if (!serverTick)
        {
            return result;
        }
        result.ServerTick = *serverTick;
        result.HeaderValid = true;

        const EntityRemap decodeRef = MakeDecodeRef(m_Map);

        while (cursor < packet.size())
        {
            const Result<u32> netId = ReadU32(packet, cursor);
            if (!netId)
            {
                break;
            }
            const Result<u32> componentCount = ReadU32(packet, cursor);
            if (!componentCount)
            {
                break;
            }

            const Entity entity = m_Map.Lookup(*netId);
            const bool known = !entity.IsNull() && scene.IsAlive(entity);

            bool applied = false;
            bool truncated = false;
            ApplyComponentRecords(packet, cursor, *componentCount, scene, entity, known, registry,
                                  decodeRef, /*bufferTransform=*/true, *serverTick, applied,
                                  truncated);

            if (!known)
            {
                ++result.EntitiesDropped;
            }
            else if (applied)
            {
                ++result.EntitiesApplied;
            }

            if (truncated)
            {
                break;
            }
        }

        return result;
    }
}
