#include "PrefabLoader.h"

#include <algorithm>
#include <cstring>

#include <fmt/format.h>

#include <Veng/Asset/AssetManager.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/HexId.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/TypeRegistry.h>

namespace Veng
{
    namespace
    {
        AssetLoadError Corrupt(AssetId id, string detail)
        {
            return AssetLoadError{
                .Kind = AssetError::Corrupt, .Id = id, .Detail = std::move(detail)};
        }

        // An embedded handle dependency: its id and the asset type its field
        // expects (mapped from the field's leaf TypeId).
        struct HandleDep
        {
            u64 Id = 0;
            AssetTypeId Type = AssetTypes::Raw;
        };

        // Collect (id, type) for every embedded AssetHandle field, recursing into struct fields.
        // Reads the AssetId off offset 0 of each handle field per the AssetHandle layout contract.
        VoidResult CollectHandleDeps(AssetId parentId, const void* obj, const TypeInfo& type,
                                     const TypeRegistry& registry,
                                     const AssetTypeRegistry& assetTypes, vector<HandleDep>& out)
        {
            for (const FieldDescriptor& field : type.Fields)
            {
                const void* fieldPtr = static_cast<const u8*>(obj) + field.Offset;

                if (field.Class == FieldClass::AssetHandle)
                {
                    u64 fid = 0;
                    std::memcpy(&fid, fieldPtr, sizeof(fid));
                    if (fid == 0)
                    {
                        continue;
                    }

                    const optional<AssetTypeId> assetType =
                        assetTypes.FindByHandleField(field.Type);
                    if (!assetType)
                    {
                        return std::unexpected(fmt::format(
                            "prefab {}: field '{}' is an AssetHandle whose leaf type {} no asset "
                            "type claims — the type's registration must set HandleFieldType",
                            parentId.Value, field.Name, FormatHexId(field.Type)));
                    }
                    out.push_back(HandleDep{.Id = fid, .Type = *assetType});
                }
                else if (field.Class == FieldClass::Struct)
                {
                    const VoidResult nested = CollectHandleDeps(
                        parentId, fieldPtr, registry.Info(field.Type), registry, assetTypes, out);
                    if (!nested)
                    {
                        return nested;
                    }
                }
                else if (field.Class == FieldClass::Variant)
                {
                    // The active alternative carries its own embedded handles (a shape's
                    // material); an empty variant has none.
                    const TypeInfo& variant = registry.Info(field.Type);
                    const TypeId active = variant.VariantActiveType(fieldPtr);
                    if (active != InvalidTypeId)
                    {
                        const VoidResult nested =
                            CollectHandleDeps(parentId, variant.VariantActivePtrConst(fieldPtr),
                                              registry.Info(active), registry, assetTypes, out);
                        if (!nested)
                        {
                            return nested;
                        }
                    }
                }
                else if (field.Class == FieldClass::Array)
                {
                    const TypeInfo& element = registry.Info(field.ElementType);
                    const usize count = field.ArraySize(fieldPtr);
                    for (usize i = 0; i < count; ++i)
                    {
                        const void* elementPtr = field.ArrayElementConst(fieldPtr, i);
                        if (element.Class == FieldClass::AssetHandle)
                        {
                            u64 fid = 0;
                            std::memcpy(&fid, elementPtr, sizeof(fid));
                            if (fid == 0)
                            {
                                continue;
                            }
                            const optional<AssetTypeId> assetType =
                                assetTypes.FindByHandleField(field.ElementType);
                            if (!assetType)
                            {
                                return std::unexpected(fmt::format(
                                    "prefab {}: array field '{}' holds AssetHandles whose leaf "
                                    "type {} no asset type claims — the type's registration must "
                                    "set HandleFieldType",
                                    parentId.Value, field.Name, FormatHexId(field.ElementType)));
                            }
                            out.push_back(HandleDep{.Id = fid, .Type = *assetType});
                        }
                        else if (element.Class == FieldClass::Struct)
                        {
                            const VoidResult nested = CollectHandleDeps(
                                parentId, elementPtr, element, registry, assetTypes, out);
                            if (!nested)
                            {
                                return nested;
                            }
                        }
                    }
                }
            }
            return {};
        }
    }

    Result<vector<Prefab::PrefabEntity>> DecodeCookedPrefab(const std::span<const u8> cooked)
    {
        if (cooked.size() < sizeof(CookedPrefabHeader))
        {
            return std::unexpected(string("prefab: cooked blob smaller than CookedPrefabHeader"));
        }

        CookedPrefabHeader header;
        std::memcpy(&header, cooked.data(), sizeof(header));

        // A stale/foreign blob is a recoverable load failure, not a crash.
        if (header.Version != CookedPrefabVersion)
        {
            return std::unexpected(
                fmt::format("prefab: blob version {} does not match expected version {}",
                            header.Version, CookedPrefabVersion));
        }

        const usize entityTableBytes =
            static_cast<usize>(header.EntityCount) * sizeof(CookedPrefabEntity);
        const usize componentTableBytes =
            static_cast<usize>(header.ComponentCount) * sizeof(CookedPrefabComponent);

        usize cursor = sizeof(CookedPrefabHeader);
        if (cooked.size() < cursor + entityTableBytes + componentTableBytes +
                                static_cast<usize>(header.RecordBytes))
        {
            return std::unexpected(string("prefab: cooked blob truncated"));
        }

        vector<CookedPrefabEntity> cookedEntities(header.EntityCount);
        if (entityTableBytes > 0)
        {
            std::memcpy(cookedEntities.data(), cooked.data() + cursor, entityTableBytes);
        }
        cursor += entityTableBytes;

        vector<CookedPrefabComponent> cookedComponents(header.ComponentCount);
        if (componentTableBytes > 0)
        {
            std::memcpy(cookedComponents.data(), cooked.data() + cursor, componentTableBytes);
        }
        cursor += componentTableBytes;

        const std::span<const u8> records = cooked.subspan(cursor, header.RecordBytes);

        vector<Prefab::PrefabEntity> entities;
        entities.reserve(header.EntityCount);
        for (const CookedPrefabEntity& ce : cookedEntities)
        {
            if (static_cast<u64>(ce.FirstComponent) + ce.ComponentCount > header.ComponentCount)
            {
                return std::unexpected(string("prefab: entity component range out of bounds"));
            }

            Prefab::PrefabEntity entity;
            entity.Components.reserve(ce.ComponentCount);
            entity.NestedPrefab = AssetId{ce.NestedPrefab};

            for (u32 c = 0; c < ce.ComponentCount; ++c)
            {
                const CookedPrefabComponent& cc = cookedComponents[ce.FirstComponent + c];
                if (static_cast<usize>(cc.RecordOffset) + cc.RecordSize > header.RecordBytes)
                {
                    return std::unexpected(string("prefab: component record range out of bounds"));
                }

                Prefab::Component component;
                component.Type = cc.TypeId;
                component.Record.assign(records.begin() + cc.RecordOffset,
                                        records.begin() + cc.RecordOffset + cc.RecordSize);
                entity.Components.push_back(std::move(component));
            }

            entities.push_back(std::move(entity));
        }
        return entities;
    }

    AssetResult<Detail::ParsedAsset> PrefabLoader::Parse(const AssetParseContext& context,
                                                         const AssetId id,
                                                         const std::span<const u8> cooked) const
    {
        const TypeRegistry& types = context.Types;
        Result<vector<Prefab::PrefabEntity>> decoded = DecodeCookedPrefab(cooked);
        if (!decoded)
        {
            return std::unexpected(Corrupt(id, std::move(decoded.error())));
        }
        auto entities = CreateRef<vector<Prefab::PrefabEntity>>(std::move(*decoded));

        // Embedded AssetHandle (id, type) pairs, surfaced as dependencies.
        vector<HandleDep> handleDeps;

        for (const Prefab::PrefabEntity& entity : *entities)
        {
            // A nesting entity's body is an ordinary load-time dependency, resolved and kept
            // resident exactly like an embedded AssetHandle field's target.
            if (entity.NestedPrefab.IsValid())
            {
                handleDeps.push_back(
                    HandleDep{.Id = entity.NestedPrefab.Value, .Type = AssetTypes::Prefab});
            }

            for (const Prefab::Component& component : entity.Components)
            {
                // Deserialize the record into a type-erased instance to walk its handle fields.
                // Skip unregistered types — they have no handle ids to contribute here;
                // spawn will assert on the missing registration later.
                if (!types.IsRegistered(component.Type))
                {
                    continue;
                }

                const TypeInfo& typeInfo = types.Info(component.Type);
                vector<u8> instance(typeInfo.Size);
                typeInfo.DefaultConstruct(instance.data());

                // The untrusted-first parse: a truncated record from a corrupt
                // cooked blob surfaces as a recoverable Corrupt load failure.
                const VoidResult read =
                    ReadFields(component.Record, instance.data(), typeInfo, types);
                if (!read)
                {
                    typeInfo.Destruct(instance.data());
                    return std::unexpected(Corrupt(id, read.error()));
                }

                const VoidResult collected = CollectHandleDeps(id, instance.data(), typeInfo, types,
                                                               context.AssetTypes, handleDeps);
                typeInfo.Destruct(instance.data());
                if (!collected)
                {
                    return std::unexpected(Corrupt(id, collected.error()));
                }
            }
        }

        // The embedded handle dependencies, deduplicated by id.
        Detail::ParsedAsset parsed;
        for (const HandleDep& dep : handleDeps)
        {
            const bool known = std::ranges::any_of(parsed.Dependencies,
                                                   [&dep](const Detail::AssetDependency& existing)
                                                   { return existing.Id.Value == dep.Id; });
            if (!known)
            {
                parsed.Dependencies.push_back({.Type = dep.Type, .Id = AssetId{dep.Id}});
            }
        }

        parsed.Complete = [entities, id](AssetManager&,
                                         std::span<const Ref<Detail::AssetCacheEntry>> resolved)
            -> AssetResult<Detail::LoadJob>
        {
            vector<Ref<Detail::AssetCacheEntry>> dependencies(resolved.begin(), resolved.end());
            const Ref<Prefab> prefab = Prefab::Create(std::move(*entities), dependencies, id);
            // The empty finalize is what holds the prefab pending until every dependency is
            // resident.
            return Detail::LoadJob{
                .Resource = Detail::RefAny(prefab),
                .Dependencies = std::move(dependencies),
                .Finalize = []() -> VoidResult { return {}; },
            };
        };
        return parsed;
    }
}
