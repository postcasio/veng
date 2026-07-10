#include <Veng/Net/Replication.h>

#include <Veng/Assert.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Result.h>
#include <Veng/Scene/Components.h>
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
        const TypeId netIdentityId = TypeIdOf<NetIdentity>();

        // Encode a replicated Entity field as its target's NetId (the reserved null id for a null or
        // unreplicated target — referencing a Local-tier entity from replicated state is an authoring
        // error). The NetId rides in the Entity's Index; the generation is unused on the wire.
        const EntityRemap encodeRef = [&scene, netIdentityId](Entity local) -> Entity
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
        const AssetHandleFixup keepAsset = [](void*) {};

        vector<u8> out;
        AppendU64(out, serverTick);

        for (auto [entity, identity] : scene.View<NetIdentity>())
        {
            vector<u8> componentRecords;
            u32 componentCount = 0;

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
                // serialize the copy — the live component is never mutated. Reusing WriteFields/
                // ReadFields and the prefab-remap walk keeps this off a forked serializer.
                vector<u8> valueCopy;
                WriteFields(valueCopy, component, info, registry);
                ScratchComponent scratch(info);
                ReadFields(valueCopy, scratch.Ptr, info, registry).value();
                RemapComponentReferences(scratch.Ptr, info, registry, encodeRef, keepAsset);

                vector<u8> payload;
                WriteFields(payload, scratch.Ptr, info, registry);

                AppendU64(componentRecords, typeId);
                AppendU32(componentRecords, static_cast<u32>(payload.size()));
                componentRecords.insert(componentRecords.end(), payload.begin(), payload.end());
                ++componentCount;
            }

            if (componentCount == 0)
            {
                continue;
            }

            AppendU32(out, identity.Id);
            AppendU32(out, componentCount);
            out.insert(out.end(), componentRecords.begin(), componentRecords.end());
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

        // Remap a wire reference (its NetId held in the Entity's Index) back through the id map; the
        // reserved null id and an unbound id both resolve to Entity::Null.
        const EntityRemap decodeRef = [&map](Entity wire) -> Entity
        {
            const NetId id = wire.Index;
            if (id == InvalidNetId)
            {
                return Entity::Null;
            }
            return map.Lookup(id);
        };
        const AssetHandleFixup keepAsset = [](void*) {};

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
            for (u32 i = 0; i < *componentCount; ++i)
            {
                const Result<u64> typeId = ReadU64(packet, cursor);
                if (!typeId)
                {
                    truncated = true;
                    break;
                }
                const Result<u32> byteLength = ReadU32(packet, cursor);
                if (!byteLength)
                {
                    truncated = true;
                    break;
                }
                if (cursor + *byteLength > packet.size())
                {
                    truncated = true;
                    break;
                }
                const std::span<const u8> payload = packet.subspan(cursor, *byteLength);
                cursor += *byteLength;

                // An unbound NetId drops the whole record, but its component records are still parsed
                // so the cursor stays aligned for the next entity record.
                if (!known)
                {
                    continue;
                }
                // An unregistered TypeId skips that component (schema-drift tolerance).
                if (!registry.IsRegistered(*typeId))
                {
                    continue;
                }

                const TypeInfo& info = registry.Info(*typeId);

                // Decode into scratch so a malformed record leaves the live component's prior state
                // intact (the entity is effectively skipped for that component, next snapshot applies).
                ScratchComponent scratch(info);
                if (VoidResult read = ReadFields(payload, scratch.Ptr, info, registry); !read)
                {
                    continue;
                }
                RemapComponentReferences(scratch.Ptr, info, registry, decodeRef, keepAsset);

                void* dest = scene.TryGetComponent(entity, *typeId);
                if (dest == nullptr)
                {
                    dest = scene.AddComponent(entity, *typeId);
                }
                // Latest-wins overwrite: destruct the prior value, move the decoded one into place.
                info.Destruct(dest);
                info.MoveConstruct(dest, scratch.Ptr);
                applied = true;
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
