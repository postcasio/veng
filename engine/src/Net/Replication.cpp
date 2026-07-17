#include <Veng/Net/Replication.h>

#include <Veng/Assert.h>
#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/Prefab.h>
#include <Veng/Log.h>
#include <Veng/Net/DeltaCodec.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Result.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/RemoteInterpolationSystem.h>
#include <Veng/Scene/Scene.h>
#include <Veng/Scene/SceneClone.h>
#include <Veng/Net/WorldEnvelope.h>

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

        // One component's live value as wire bytes: the reflection serializer's WriteFields form with
        // its Entity references translated to NetIds, computed out of line so the live component is
        // never mutated. This is the "current value" the delta codec deltas and the client reconstructs.
        vector<u8> EncodeComponentWireBytes(const Scene& scene, Entity entity, TypeId typeId,
                                            const TypeInfo& info, const TypeRegistry& registry,
                                            const EntityRemap& encodeRef)
        {
            const AssetHandleFixup keepAsset = [](void*) {};
            const void* component = scene.TryGetComponent(entity, typeId);

            vector<u8> valueCopy;
            WriteFields(valueCopy, component, info, registry);
            ScratchComponent scratch(info);
            ReadFields(valueCopy, scratch.Ptr, info, registry).value();
            RemapComponentReferences(scratch.Ptr, info, registry, encodeRef, keepAsset);

            vector<u8> wire;
            WriteFields(wire, scratch.Ptr, info, registry);
            return wire;
        }

        // Appends one component record (TypeId:u64 ByteLength:u32 <encoding tag + body>) to @p out,
        // deltaing against @p baseline when one is shared (empty ⇒ the full self-describing form).
        void AppendComponentRecord(vector<u8>& out, TypeId typeId, std::span<const u8> currentBytes,
                                   std::span<const u8> baseline, bool forceFull,
                                   TypeId transformType, const TypeRegistry& registry,
                                   const Net::QuantizationSettings& quant)
        {
            vector<u8> body;
            Net::EncodeComponentBody(body, typeId, currentBytes, baseline, forceFull, transformType,
                                     registry, quant);
            AppendU64(out, typeId);
            AppendU32(out, static_cast<u32>(body.size()));
            out.insert(out.end(), body.begin(), body.end());
        }

        // One entity's dirty replicated components as full self-describing records — the spawn/baseline
        // form and the free EncodeSnapshot form (no per-connection baseline, no quantization).
        EncodedComponents EncodeDirtyComponents(const Scene& scene, Entity entity,
                                                const vector<TypeId>& replicated, u64 sinceTick,
                                                const TypeRegistry& registry,
                                                const EntityRemap& encodeRef)
        {
            EncodedComponents result;
            for (const TypeId typeId : replicated)
            {
                if (scene.TryGetComponent(entity, typeId) == nullptr)
                {
                    continue;
                }
                if (scene.GetComponentChangeTick(entity, typeId) <= sinceTick)
                {
                    continue;
                }
                const TypeInfo& info = registry.Info(typeId);
                const vector<u8> wire =
                    EncodeComponentWireBytes(scene, entity, typeId, info, registry, encodeRef);
                AppendComponentRecord(result.Bytes, typeId, wire, {}, /*forceFull=*/false,
                                      InvalidTypeId, registry, Net::QuantizationSettings{});
                ++result.Count;
            }
            return result;
        }

        // The per-(NetId, TypeId) baseline store the delta decoder patches against and updates. Null
        // on the stateless free ApplySnapshot (which only ever sees full self-describing records).
        using BaselineStore = unordered_map<NetId, unordered_map<TypeId, vector<u8>>>;

        // Applies one entity record's component list (positioned at the first component record).
        // Each component body opens with an encoding tag: a full record reconstructs the value, a
        // delta patches @p baselines[netId][type], and a quantized Transform dequantizes — all out of
        // line, so a malformed record leaves prior state intact. When the record is for a known
        // (bound, alive) entity, each decoded component either buffers a Transform sample
        // (bufferTransform), collects the authoritative state into @p collect (a predicted entity,
        // handed to the reconciler rather than applied), or overwrites the live component; an unknown
        // entity's records are parsed only to keep the cursor aligned. Stops on a truncated record.
        void ApplyComponentRecords(std::span<const u8> packet, usize& cursor, u32 componentCount,
                                   Scene& scene, Entity entity, NetId netId, bool known,
                                   const TypeRegistry& registry, const EntityRemap& decodeRef,
                                   bool bufferTransform, u64 serverTick,
                                   const Net::QuantizationSettings& quant, BaselineStore* baselines,
                                   bool& appliedOut, bool& truncatedOut,
                                   PredictedRecord* collect = nullptr,
                                   vector<TypeId>* addedTypes = nullptr)
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
                const std::span<const u8> body = packet.subspan(cursor, *byteLength);
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

                // Decode the body against this connection's baseline for the (entity, component),
                // yielding the full wire bytes; store them as the new baseline for the next delta.
                std::span<const u8> baseline;
                if (baselines != nullptr)
                {
                    const auto entityIt = baselines->find(netId);
                    if (entityIt != baselines->end())
                    {
                        const auto compIt = entityIt->second.find(*typeId);
                        if (compIt != entityIt->second.end())
                        {
                            baseline = compIt->second;
                        }
                    }
                }
                vector<u8> payload;
                if (VoidResult decoded = Net::DecodeComponentBody(
                        body, baseline, *typeId, transformId, registry, quant, payload);
                    !decoded)
                {
                    continue; // baseline mismatch / malformed: leave prior state intact
                }
                if (baselines != nullptr)
                {
                    (*baselines)[netId][*typeId] = payload;
                }

                ScratchComponent scratch(info);
                if (VoidResult read = ReadFields(payload, scratch.Ptr, info, registry); !read)
                {
                    continue; // malformed record: leave prior state intact
                }
                RemapComponentReferences(scratch.Ptr, info, registry, decodeRef, keepAsset);

                if (collect != nullptr)
                {
                    // A predicted entity: hand the decoded authoritative state (local-form, references
                    // remapped) to the reconciler instead of writing it onto the client-simulated pose.
                    vector<u8> local;
                    WriteFields(local, scratch.Ptr, info, registry);
                    collect->Components.push_back(
                        PredictedRecord::Component{.Type = *typeId, .Bytes = std::move(local)});
                    appliedOut = true;
                    continue;
                }

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
                    // Absent before this record: the stream is adding the component. An adoption pass
                    // notes it (addedTypes) so leave/despawn removes exactly the types the stream added,
                    // keeping the claimant's pre-existing components at their last values.
                    if (addedTypes != nullptr)
                    {
                        addedTypes->push_back(*typeId);
                    }
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

    vector<u8> EncodeSnapshot(const Scene& scene, u64 serverTick, u64 sinceTick, i32 inputFeedback,
                              u64 lastConsumedInputTick)
    {
        const TypeRegistry& registry = scene.GetTypeRegistry();
        const vector<TypeId> replicated = ReplicatedTypeIds(registry);
        const EntityRemap encodeRef = MakeEncodeRef(scene);

        vector<u8> out;
        AppendU64(out, serverTick);
        AppendU32(out, static_cast<u32>(inputFeedback));
        AppendU64(out, lastConsumedInputTick);

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
        const Result<u32> feedback = ReadU32(packet, cursor);
        if (!feedback)
        {
            return result;
        }
        const Result<u64> lastConsumed = ReadU64(packet, cursor);
        if (!lastConsumed)
        {
            return result;
        }
        result.ServerTick = *serverTick;
        result.InputFeedback = static_cast<i32>(*feedback);
        result.LastConsumedInputTick = *lastConsumed;
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
            ApplyComponentRecords(packet, cursor, *componentCount, scene, entity, *netId, known,
                                  registry, decodeRef, /*bufferTransform=*/false, *serverTick,
                                  Net::QuantizationSettings{}, /*baselines=*/nullptr, applied,
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

    void ReplicationServer::SetInputFeedback(Net::ConnectionId id, i32 feedback)
    {
        if (const auto it = m_Connections.find(id); it != m_Connections.end())
        {
            it->second.InputFeedback = feedback;
        }
    }

    void ReplicationServer::SetLastConsumedInputTick(Net::ConnectionId id, u64 tick)
    {
        if (const auto it = m_Connections.find(id); it != m_Connections.end())
        {
            it->second.LastConsumedInputTick = tick;
        }
    }

    vector<ReplicationMessage> ReplicationServer::Generate(Net::ConnectionId id, const Scene& scene,
                                                           u64 tick, const set<NetId>* interest)
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

        // An entity is relevant to this connection when interest is off (null) or it is in the set.
        const auto relevant = [interest](NetId netId)
        { return interest == nullptr || interest->contains(netId); };

        // Diff the relevant replicated set against what this connection already has: a Spawn for each
        // new relevant NetId, a Despawn for each spawned one now gone (destroyed) or no longer relevant
        // (a visibility exit). NetIdentity marks exactly the server-authoritative set.
        set<NetId> live;
        for (auto [entity, identity] : scene.View<NetIdentity>())
        {
            if (identity.Id == InvalidNetId)
            {
                continue;
            }
            live.insert(identity.Id);

            if (state.Spawned.contains(identity.Id) || !relevant(identity.Id))
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

            // An anchored entity replicates its opaque anchor in the spawn record (beside the prefab
            // id), read before the client creates any entity so the claimant resolves at spawn time.
            const auto* anchor =
                static_cast<const NetAnchor*>(scene.TryGetComponent(entity, TypeIdOf<NetAnchor>()));

            vector<u8> spawn;
            AppendU8(spawn, static_cast<u8>(ReplicationMessageId::Spawn));
            AppendU32(spawn, identity.Id);
            AppendU32(spawn, owner);
            AppendU8(spawn, hasPrefab ? 1 : 0);
            if (hasPrefab)
            {
                AppendU64(spawn, prefabIt->second.Value);
            }
            AppendU8(spawn, anchor != nullptr ? 1 : 0);
            if (anchor != nullptr)
            {
                AppendU64(spawn, anchor->Lo);
                AppendU64(spawn, anchor->Hi);
            }
            AppendU32(spawn, encoded.Count);
            spawn.insert(spawn.end(), encoded.Bytes.begin(), encoded.Bytes.end());
            messages.push_back(ReplicationMessage{.Channel = Net::Channel::ReliableOrdered,
                                                  .Bytes = std::move(spawn)});
        }

        // Despawns: a spawned NetId no longer live is destroyed; one still live but no longer relevant
        // is a visibility exit (re-entry re-spawns and re-baselines it).
        vector<std::pair<NetId, DespawnReason>> gone;
        for (const NetId spawned : state.Spawned)
        {
            if (!live.contains(spawned))
            {
                gone.emplace_back(spawned, DespawnReason::Destroyed);
            }
            else if (!relevant(spawned))
            {
                gone.emplace_back(spawned, DespawnReason::Visibility);
            }
        }
        for (const auto& [netId, reason] : gone)
        {
            state.Spawned.erase(netId);
            // Drop the connection's delta baseline for the entity; a re-spawn re-bases from its spawn.
            state.Baseline.erase(netId);
            vector<u8> despawn;
            AppendU8(despawn, static_cast<u8>(ReplicationMessageId::Despawn));
            AppendU32(despawn, netId);
            AppendU8(despawn, static_cast<u8>(reason));
            messages.push_back(ReplicationMessage{.Channel = Net::Channel::ReliableOrdered,
                                                  .Bytes = std::move(despawn)});
        }

        // Snapshot on the interval tick: pack each connection's dirty state as ack-keyed field deltas
        // (quantized spatial leaves) into MTU-sized unreliable packets, each a self-contained snapshot.
        // The gate is the world's own sim tick, so a world ticking below the host pump rate holds one
        // qualifying tick across many pumps; emitting only when the tick has advanced past the last
        // snapshot keeps the cadence one snapshot per qualifying world tick, not one per pump.
        if (m_Settings.SnapshotInterval != 0 && tick % m_Settings.SnapshotInterval == 0 &&
            tick != state.LastSnapshotTick)
        {
            state.LastSnapshotTick = tick;
            // Advance the delta baseline to the acked tick by adopting the sent states the connection
            // has now acknowledged; the newest adopted entry carries every still-dirty component's
            // value (send-until-acked), so dropping the older window entries loses nothing.
            if (state.AckedTick > state.AdoptedTick)
            {
                for (const SentSnapshot& sent : state.InFlight)
                {
                    if (sent.Tick > state.AckedTick)
                    {
                        break; // InFlight is ascending; the rest is still unacked
                    }
                    for (const auto& [nid, comps] : sent.Components)
                    {
                        for (const auto& [tid, bytes] : comps)
                        {
                            state.Baseline[nid][tid] = bytes;
                        }
                    }
                }
                std::erase_if(state.InFlight, [&](const SentSnapshot& sent)
                              { return sent.Tick <= state.AckedTick; });
                state.AdoptedTick = state.AckedTick;
            }

            ++state.SnapshotCounter;
            const bool keyframe = m_Settings.KeyframeInterval != 0 &&
                                  state.SnapshotCounter % m_Settings.KeyframeInterval == 0;
            // Quantization is server-opt-in: with it off, Transform rides the lossless field-delta
            // path (InvalidTypeId disables the quantized leaf encoding), so a byte-exact consumer of
            // the wire stays exact; with it on, Transform's leaves quantize.
            const TypeId transformType =
                m_Settings.QuantizeSpatial ? TypeIdOf<Transform>() : InvalidTypeId;

            SentSnapshot sent;
            sent.Tick = tick;

            const auto startPacket =
                [tick, feedback = state.InputFeedback, lastConsumed = state.LastConsumedInputTick]()
            {
                vector<u8> packet;
                AppendU64(packet, tick);
                AppendU32(packet, static_cast<u32>(feedback));
                AppendU64(packet, lastConsumed);
                return packet;
            };

            vector<u8> current = startPacket();
            bool currentHasRecords = false;

            for (auto [entity, identity] : scene.View<NetIdentity>())
            {
                if (!relevant(identity.Id))
                {
                    continue;
                }
                vector<u8> comps;
                u32 count = 0;
                for (const TypeId typeId : replicated)
                {
                    if (scene.TryGetComponent(entity, typeId) == nullptr)
                    {
                        continue;
                    }
                    if (scene.GetComponentChangeTick(entity, typeId) <= state.AckedTick)
                    {
                        continue;
                    }
                    const TypeInfo& info = registry.Info(typeId);
                    const vector<u8> wire =
                        EncodeComponentWireBytes(scene, entity, typeId, info, registry, encodeRef);

                    std::span<const u8> baseline;
                    if (const auto entIt = state.Baseline.find(identity.Id);
                        entIt != state.Baseline.end())
                    {
                        if (const auto compIt = entIt->second.find(typeId);
                            compIt != entIt->second.end())
                        {
                            baseline = compIt->second;
                        }
                    }

                    AppendComponentRecord(comps, typeId, wire, baseline, keyframe, transformType,
                                          registry, m_Settings.Quantization);
                    sent.Components[identity.Id][typeId] = wire;
                    ++count;
                }
                if (count == 0)
                {
                    continue;
                }

                vector<u8> record;
                AppendU32(record, identity.Id);
                AppendU32(record, count);
                record.insert(record.end(), comps.begin(), comps.end());

                if (currentHasRecords &&
                    current.size() + record.size() > Net::MaxEnvelopedUnreliablePayload)
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

            // Retain this snapshot's sent state until acked, bounded by the unacked window.
            state.InFlight.push_back(std::move(sent));
            constexpr usize MaxInFlight = 128;
            if (state.InFlight.size() > MaxInFlight)
            {
                state.InFlight.erase(state.InFlight.begin());
            }
        }

        return messages;
    }

    Net::JoinId AnchorBindings::OwnerOf(u64 lo, u64 hi) const
    {
        const auto it = m_Owners.find({lo, hi});
        return it != m_Owners.end() ? it->second : Net::ControlJoinId;
    }

    void AnchorBindings::Bind(u64 lo, u64 hi, Net::JoinId join)
    {
        m_Owners[{lo, hi}] = join;
    }

    void AnchorBindings::Release(u64 lo, u64 hi)
    {
        m_Owners.erase({lo, hi});
    }

    ReplicationClient::ReplicationClient(function<Ref<Prefab>(AssetId)> resolvePrefab)
        : m_ResolvePrefab(std::move(resolvePrefab))
    {
    }

    void ReplicationClient::SetAdoption(Net::JoinId join, AnchorBindings& bindings)
    {
        m_Join = join;
        m_Anchors = &bindings;
    }

    void ReplicationClient::ReleaseAdopted(Scene& scene, NetId id)
    {
        const auto it = m_Adopted.find(id);
        if (it == m_Adopted.end())
        {
            return;
        }
        const AdoptedEntity adopted = std::move(it->second);
        m_Adopted.erase(it);

        // Remove exactly the component types the stream added at bind; the claimant's pre-existing
        // components keep their last-applied values, and the entity itself is never destroyed.
        if (!adopted.Claimant.IsNull() && scene.IsAlive(adopted.Claimant))
        {
            for (const TypeId type : adopted.AddedTypes)
            {
                scene.RemoveComponent(adopted.Claimant, type);
            }
        }
        // Free the anchor for the next join to bind, and drop the wire id's map + baseline state.
        m_Anchors->Release(adopted.AnchorLo, adopted.AnchorHi);
        m_Map.Unbind(id);
        m_Baseline.erase(id);
    }

    void ReplicationClient::Leave(Scene& scene)
    {
        // Release every adopted claimant (kept alive) and destroy every wire-owned entity. Snapshot the
        // map first: DestroyEntity is recursive and mutates the scene, and ReleaseAdopted mutates the
        // maps being walked.
        const vector<std::pair<NetId, Entity>> bindings(m_Map.Bindings().begin(),
                                                        m_Map.Bindings().end());
        for (const auto& [id, entity] : bindings)
        {
            if (m_Adopted.contains(id))
            {
                ReleaseAdopted(scene, id);
                continue;
            }
            if (!entity.IsNull() && scene.IsAlive(entity))
            {
                scene.DestroyEntity(entity);
            }
        }
        m_Map.Clear();
        m_Baseline.clear();
        m_Adopted.clear();
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
            const Result<u8> hasAnchor = ReadU8(message, cursor);
            if (!hasAnchor)
            {
                return result;
            }
            u64 anchorLo = 0;
            u64 anchorHi = 0;
            if (*hasAnchor != 0)
            {
                const Result<u64> lo = ReadU64(message, cursor);
                const Result<u64> hi = ReadU64(message, cursor);
                if (!lo || !hi)
                {
                    return result;
                }
                anchorLo = *lo;
                anchorHi = *hi;
            }
            const Result<u32> componentCount = ReadU32(message, cursor);
            if (!componentCount)
            {
                return result;
            }

            const EntityRemap decodeRef = MakeDecodeRef(m_Map);
            bool applied = false;
            bool truncated = false;

            // An anchored spawn binds to a live local claimant carrying the equal anchor instead of
            // spawning a duplicate. Resolve the claimant from the per-scene View<NetAnchor>; two matches
            // is ambiguous API misuse (adoption cannot pick — a fatal assert), one is the claimant, none
            // falls through to an ordinary wire-owned spawn with a one-shot warning.
            if (*hasAnchor != 0)
            {
                Entity claimant = Entity::Null;
                u32 matches = 0;
                for (auto [entity, netAnchor] : scene.View<NetAnchor>())
                {
                    if (netAnchor.Lo == anchorLo && netAnchor.Hi == anchorHi)
                    {
                        claimant = entity;
                        ++matches;
                    }
                }
                VE_ASSERT(matches <= 1,
                          "two live claimants of anchor {:016X}{:016X} in one scene — adoption is "
                          "ambiguous",
                          anchorHi, anchorLo);

                if (matches == 1)
                {
                    // Single-source: exactly one live join binds a claimant at a time. A different join
                    // already binding this anchor is the tripwire on that invariant (a fatal assert);
                    // the same join re-binding is an idempotent re-spawn.
                    const Net::JoinId owner = m_Anchors->OwnerOf(anchorLo, anchorHi);
                    VE_ASSERT(
                        owner == Net::ControlJoinId || owner == m_Join,
                        "anchor {:016X}{:016X} is already bound by join {}; a second live join "
                        "binding it violates single-source adoption",
                        anchorHi, anchorLo, owner);
                    m_Anchors->Bind(anchorLo, anchorHi, m_Join);

                    // Apply the record's components onto the claimant, recording which types the stream
                    // added (absent before) so the release removes exactly those and keeps the rest.
                    vector<TypeId> added;
                    ApplyComponentRecords(message, cursor, *componentCount, scene, claimant, *netId,
                                          /*known=*/true, registry, decodeRef,
                                          /*bufferTransform=*/false, /*serverTick=*/0,
                                          m_Quantization, &m_Baseline, applied, truncated,
                                          /*collect=*/nullptr, &added);
                    (void)applied;
                    (void)truncated;

                    // Route the wire id to the claimant; the derived entity keeps its Local authority
                    // and identity — only the replicated state layers on.
                    m_Map.Bind(*netId, claimant);
                    m_Adopted[*netId] = AdoptedEntity{.Claimant = claimant,
                                                      .AnchorLo = anchorLo,
                                                      .AnchorHi = anchorHi,
                                                      .AddedTypes = std::move(added)};
                    result.Spawned = true;
                    result.Adopted = true;
                    result.Id = *netId;
                    result.Entity = claimant;
                    return result;
                }

                if (!m_WarnedClaimantMiss)
                {
                    Log::Warn(
                        "Anchored spawn {} found no local claimant for anchor {:016X}{:016X}; "
                        "spawning wire-owned (derivation drift?)",
                        *netId, anchorHi, anchorLo);
                    m_WarnedClaimantMiss = true;
                }
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

            ApplyComponentRecords(message, cursor, *componentCount, scene, entity, *netId,
                                  /*known=*/true, registry, decodeRef, /*bufferTransform=*/false,
                                  /*serverTick=*/0, m_Quantization, &m_Baseline, applied,
                                  truncated);
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
            // The reason rides after the id (a legacy id-only despawn defaults to Destroyed). The
            // client teardown is side-effect-free either way; the reason is surfaced for game logic,
            // which must not fire death on a visibility exit.
            if (const Result<u8> reason = ReadU8(message, cursor))
            {
                result.Reason = static_cast<DespawnReason>(*reason);
            }
            const Entity entity = m_Map.Lookup(*netId);
            // An adopted claimant is released, never destroyed: the derived entity outlives the binding
            // (re-adoptable by the next join), with only the stream-added state removed.
            if (m_Adopted.contains(*netId))
            {
                result.Entity = entity;
                ReleaseAdopted(scene, *netId);
                result.Despawned = true;
                return result;
            }
            if (!entity.IsNull() && scene.IsAlive(entity))
            {
                scene.DestroyEntity(entity);
                result.Entity = entity;
            }
            m_Map.Unbind(*netId);
            // Drop the entity's delta baseline: a re-spawn of the same id re-bases from its spawn
            // record, so a stale baseline can never patch the wrong entity's state.
            m_Baseline.erase(*netId);
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
        m_PredictedRecords.clear();

        usize cursor = 0;
        const Result<u64> serverTick = ReadU64(packet, cursor);
        if (!serverTick)
        {
            return result;
        }
        const Result<u32> feedback = ReadU32(packet, cursor);
        if (!feedback)
        {
            return result;
        }
        const Result<u64> lastConsumed = ReadU64(packet, cursor);
        if (!lastConsumed)
        {
            return result;
        }
        result.ServerTick = *serverTick;
        result.InputFeedback = static_cast<i32>(*feedback);
        result.LastConsumedInputTick = *lastConsumed;
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

            // A predicted entity is simulated locally, so its authoritative state is neither buffered
            // (as a remote mirror's Transform is) nor applied latest-wins over the client-driven pose.
            // Its records are collected for the reconciler, which compares them against the recorded
            // prediction at the header's consumed-input tick and restores/replays on a mismatch.
            const Authority* authority = known ? scene.TryGet<Authority>(entity) : nullptr;
            const bool predicted = authority != nullptr && authority->Tier == Tier::Predicted;

            PredictedRecord collected;
            collected.Entity = entity;

            bool applied = false;
            bool truncated = false;
            ApplyComponentRecords(packet, cursor, *componentCount, scene, entity, *netId, known,
                                  registry, decodeRef, /*bufferTransform=*/!predicted, *serverTick,
                                  m_Quantization, &m_Baseline, applied, truncated,
                                  predicted ? &collected : nullptr);

            if (predicted && !collected.Components.empty())
            {
                m_PredictedRecords.push_back(std::move(collected));
            }

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
