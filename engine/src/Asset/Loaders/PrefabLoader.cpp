#include "PrefabLoader.h"

#include <cstring>

#include <fmt/format.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Texture.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/TypeRegistry.h>

namespace Veng
{
    namespace
    {
        AssetLoadError Corrupt(AssetId id, string detail)
        {
            return AssetLoadError{.Kind = AssetError::Corrupt, .Id = id, .Detail = std::move(detail)};
        }

        // The AssetType an embedded AssetHandle<T> field loads as, keyed by the
        // field's leaf TypeId — the same mapping the cooker validates against.
        optional<AssetType> AssetTypeForHandleField(TypeId fieldType)
        {
            if (fieldType == TypeIdOf<AssetHandle<Texture>>())  return AssetType::Texture;
            if (fieldType == TypeIdOf<AssetHandle<Mesh>>())     return AssetType::Mesh;
            if (fieldType == TypeIdOf<AssetHandle<Material>>()) return AssetType::Material;
            return std::nullopt;
        }

        // Load one embedded dependency by id + type, returning its cache entry.
        // Mirrors a Material's texture/shader sub-load: async fans out a
        // non-blocking Load, sync blocks on LoadSync. The Prefab keeps the entry
        // resident for its lifetime so SpawnInto's rehydration is a cache hit.
        AssetResult<Ref<Detail::AssetCacheEntry>> LoadDependency(
            AssetManager& manager, AssetId parentId, AssetId depId, AssetType type, bool async)
        {
            auto load = [&]<class T>() -> AssetResult<Ref<Detail::AssetCacheEntry>>
            {
                if (async)
                {
                    AssetHandle<T> handle = manager.Load<T>(depId);
                    if (!AssetManager::EntryOf(handle))
                    {
                        return std::unexpected(AssetLoadError{
                            .Kind = AssetError::MissingDependency, .Id = depId,
                            .Detail = fmt::format("prefab {}: dependency {} did not resolve",
                                                  parentId.Value, depId.Value)});
                    }
                    return AssetManager::EntryOf(handle);
                }

                const AssetResult<AssetHandle<T>> handle = manager.LoadSync<T>(depId);
                if (!handle)
                    return std::unexpected(handle.error());
                return AssetManager::EntryOf(*handle);
            };

            switch (type)
            {
                case AssetType::Texture:  return load.operator()<Texture>();
                case AssetType::Mesh:     return load.operator()<Mesh>();
                case AssetType::Material: return load.operator()<Material>();
                default:
                    return std::unexpected(Corrupt(parentId, fmt::format(
                        "prefab {}: embedded handle field has unsupported asset type {}",
                        parentId.Value, static_cast<u32>(type))));
            }
        }

        // An embedded handle dependency: its id and the asset type its field
        // expects (mapped from the field's leaf TypeId).
        struct HandleDep
        {
            u64 Id = 0;
            AssetType Type = AssetType::Raw;
        };

        // Walk a value's reflected fields, collecting every embedded AssetHandle
        // field's (id, expected type) (recursing into nested struct fields). Reads
        // the id off offset 0 of each handle field, per the AssetHandle layout
        // guard.
        VoidResult CollectHandleDeps(AssetId parentId, const void* obj, const TypeInfo& type,
                                     const TypeRegistry& registry, vector<HandleDep>& out)
        {
            for (const FieldDescriptor& field : type.Fields)
            {
                const void* fieldPtr = static_cast<const u8*>(obj) + field.Offset;

                if (field.Class == FieldClass::AssetHandle)
                {
                    u64 fid = 0;
                    std::memcpy(&fid, fieldPtr, sizeof(fid));
                    if (fid == 0)
                        continue;

                    const optional<AssetType> assetType = AssetTypeForHandleField(field.Type);
                    if (!assetType)
                    {
                        return std::unexpected(fmt::format(
                            "prefab {}: field '{}' is an AssetHandle of an unrecognized asset type",
                            parentId.Value, field.Name));
                    }
                    out.push_back(HandleDep{.Id = fid, .Type = *assetType});
                }
                else if (field.Class == FieldClass::Struct)
                {
                    const VoidResult nested =
                        CollectHandleDeps(parentId, fieldPtr, registry.Info(field.Type), registry, out);
                    if (!nested)
                        return nested;
                }
            }
            return {};
        }
    }

    AssetResult<Detail::LoadJob> PrefabLoader::Load(
        AssetManager& manager, Renderer::Context& /*context*/, TaskSystem& /*tasks*/,
        TypeRegistry& types, AssetId id, std::span<const u8> cooked, bool async) const
    {
        // ── 1. CookedPrefabHeader ────────────────────────────────────────────
        if (cooked.size() < sizeof(CookedPrefabHeader))
            return std::unexpected(Corrupt(id, "prefab: cooked blob smaller than CookedPrefabHeader"));

        CookedPrefabHeader header;
        std::memcpy(&header, cooked.data(), sizeof(header));

        // A stale/foreign blob is a recoverable load failure, not a crash.
        if (header.Version != CookedPrefabVersion)
        {
            return std::unexpected(Corrupt(id, fmt::format(
                "prefab: blob version {} does not match expected version {}",
                header.Version, CookedPrefabVersion)));
        }

        const usize entityTableBytes = static_cast<usize>(header.EntityCount) * sizeof(CookedPrefabEntity);
        const usize componentTableBytes =
            static_cast<usize>(header.ComponentCount) * sizeof(CookedPrefabComponent);

        usize cursor = sizeof(CookedPrefabHeader);
        if (cooked.size() < cursor + entityTableBytes + componentTableBytes
                + static_cast<usize>(header.RecordBytes))
        {
            return std::unexpected(Corrupt(id, "prefab: cooked blob truncated"));
        }

        // ── 2. Entity + component tables ─────────────────────────────────────
        vector<CookedPrefabEntity> cookedEntities(header.EntityCount);
        if (entityTableBytes > 0)
            std::memcpy(cookedEntities.data(), cooked.data() + cursor, entityTableBytes);
        cursor += entityTableBytes;

        vector<CookedPrefabComponent> cookedComponents(header.ComponentCount);
        if (componentTableBytes > 0)
            std::memcpy(cookedComponents.data(), cooked.data() + cursor, componentTableBytes);
        cursor += componentTableBytes;

        const std::span<const u8> records = cooked.subspan(cursor, header.RecordBytes);

        // ── 3. Build the decoded value tree (records kept verbatim) ──────────
        vector<Prefab::PrefabEntity> entities;
        entities.reserve(header.EntityCount);

        // Embedded AssetHandle (id, type) pairs, surfaced as dependencies.
        vector<HandleDep> handleDeps;

        for (u32 e = 0; e < header.EntityCount; ++e)
        {
            const CookedPrefabEntity& ce = cookedEntities[e];

            if (ce.FirstComponent + ce.ComponentCount > header.ComponentCount)
                return std::unexpected(Corrupt(id, "prefab: entity component range out of bounds"));

            Prefab::PrefabEntity entity;
            entity.Components.reserve(ce.ComponentCount);

            for (u32 c = 0; c < ce.ComponentCount; ++c)
            {
                const CookedPrefabComponent& cc = cookedComponents[ce.FirstComponent + c];

                if (static_cast<usize>(cc.RecordOffset) + cc.RecordSize > header.RecordBytes)
                    return std::unexpected(Corrupt(id, "prefab: component record range out of bounds"));

                Prefab::Component component;
                component.Type = cc.TypeId;
                component.Record.assign(
                    records.begin() + cc.RecordOffset,
                    records.begin() + cc.RecordOffset + cc.RecordSize);

                // Extract embedded handle dependencies by deserializing the record
                // into a type-erased instance (the same lifecycle thunks the cooker
                // uses) and walking its descriptors. A component whose type the
                // module does not register is a registry mismatch — fatal at spawn,
                // but here we only need its handle ids, which a missing type cannot
                // contribute, so skip it (spawn will assert).
                if (types.IsRegistered(cc.TypeId))
                {
                    const TypeInfo& typeInfo = types.Info(cc.TypeId);
                    vector<u8> instance(typeInfo.Size);
                    typeInfo.DefaultConstruct(instance.data());
                    ReadFields(component.Record, instance.data(), typeInfo, types);
                    const VoidResult collected =
                        CollectHandleDeps(id, instance.data(), typeInfo, types, handleDeps);
                    typeInfo.Destruct(instance.data());
                    if (!collected)
                        return std::unexpected(Corrupt(id, collected.error()));
                }

                entity.Components.push_back(std::move(component));
            }

            entities.push_back(std::move(entity));
        }

        // ── 4. Fan out embedded handle dependencies (deduplicated by id) ─────
        vector<Ref<Detail::AssetCacheEntry>> dependencies;
        vector<u64> loaded;
        for (const HandleDep& dep : handleDeps)
        {
            bool known = false;
            for (const u64 existing : loaded)
                if (existing == dep.Id) { known = true; break; }
            if (known)
                continue;
            loaded.push_back(dep.Id);

            AssetResult<Ref<Detail::AssetCacheEntry>> entry =
                LoadDependency(manager, id, AssetId{dep.Id}, dep.Type, async);
            if (!entry)
                return std::unexpected(entry.error());
            dependencies.push_back(*entry);
        }

        // ── 5. Construct the Prefab ──────────────────────────────────────────
        const Ref<Prefab> prefab = Prefab::Create(std::move(entities), dependencies);

        // The prefab is CPU data — no GPU resource, no bindless registration. The
        // trivial finalize exists only so the async path orders the embedded
        // dependencies before the prefab becomes resident, uniform with every
        // dependent asset.
        return Detail::LoadJob{
            .Resource = Detail::RefAny(prefab),
            .Dependencies = std::move(dependencies),
            .Finalize = []() -> VoidResult { return {}; },
        };
    }
}
