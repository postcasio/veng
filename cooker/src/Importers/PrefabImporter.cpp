#include "PrefabImporter.h"

#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>

#include <cstring>
#include <fstream>
#include <sstream>

#include <fmt/format.h>

#include <Veng/Asset/AssetHandleType.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Cook/JsonFile.h>
#include <Veng/Reflection/JsonSerialize.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Scene/Entity.h>

namespace Veng::Cook
{
    namespace
    {
        // Whether `actual` is an acceptable source type for an AssetHandle field expecting
        // `expected`. The default-instance rule lets a MaterialInstance field accept a bare
        // Material id (resolved to the parent's zero-override default instance at load).
        bool AssetTypeAccepted(AssetType expected, AssetType actual)
        {
            if (actual == expected)
            {
                return true;
            }
            return expected == AssetType::MaterialInstance && actual == AssetType::Material;
        }

        // Located-error prefix for a field within an entity's component.
        string Located(const string& file, usize entityIndex, const string& entityName,
                       const string& typeName, const string& reason)
        {
            return fmt::format("prefab importer: '{}': entity[{}] '{}' component '{}': {}", file,
                               entityIndex, entityName, typeName, reason);
        }

        // Builds the JsonFieldHooks for one component bind: AssetId validation against the
        // pack resolver, and Reference resolution to a prefab-local entity index.
        JsonFieldHooks MakeHooks(usize entityCount,
                                 const function<optional<ResolvedSource>(AssetId)>& resolve)
        {
            JsonFieldHooks hooks;

            hooks.ValidateAssetId = [resolve](u64 id, TypeId fieldType) -> VoidResult
            {
                const optional<AssetType> expected = AssetTypeForHandleField(fieldType);
                const optional<ResolvedSource> resolved = resolve(AssetId{.Value = id});
                // Resolve only validates ids present in this pack (or a --reference pack);
                // a non-resident id is accepted as-is (residency is the runtime's job).
                if (resolved && expected && !AssetTypeAccepted(*expected, resolved->Type))
                {
                    return std::unexpected(
                        fmt::format("asset {} resolves to type {} but the field expects type {}",
                                    id, ToString(resolved->Type), ToString(*expected)));
                }
                return {};
            };

            hooks.ReadReference = [entityCount](const json& value) -> Result<Entity>
            {
                if (!value.is_number_unsigned())
                {
                    return std::unexpected(string("expected an unsigned entity index or null"));
                }

                const u64 index = value.get<u64>();
                if (index >= entityCount)
                {
                    return std::unexpected(
                        fmt::format("entity reference index {} is out of range (prefab has {} "
                                    "entities)",
                                    index, entityCount));
                }

                // The cooked reference stores the prefab-local index in Index, Generation 0;
                // the loader remaps it to the spawned handle.
                return Entity{.Index = static_cast<u32>(index), .Generation = 0};
            };

            return hooks;
        }

        template <class T>
        void Append(vector<u8>& out, const T& value)
        {
            const auto* p = reinterpret_cast<const u8*>(&value);
            out.insert(out.end(), p, p + sizeof(T));
        }
    }

    Result<vector<u8>> PrefabImporter::Cook(const CookContext& context, const json& entry) const
    {
        // --- 0. The reflected registry (--module) is required ---

        if (context.Types == nullptr)
        {
            return std::unexpected("prefab cooking requires --module");
        }

        const TypeRegistry& registry = *context.Types;

        // --- 1. Read + parse the external *.prefab.json ---

        if (!entry.contains("source") || !entry["source"].is_string())
        {
            return std::unexpected("prefab importer: missing or invalid 'source'");
        }

        const path prefabPath = context.PackDir / entry["source"].get<string>();
        const string file = prefabPath.string();

        const Result<json> prefabResult = ReadJsonFile(prefabPath, "prefab importer");
        if (!prefabResult)
        {
            return std::unexpected(prefabResult.error());
        }
        const json& prefab = *prefabResult;

        if (!prefab.contains("entities") || !prefab["entities"].is_array())
        {
            return std::unexpected(
                fmt::format("prefab importer: '{}': missing or invalid 'entities' array", file));
        }

        const json& entities = prefab["entities"];
        const usize entityCount = entities.size();

        // Resolve closure (may be unset for a pack with no resolvable references).
        const function<optional<ResolvedSource>(AssetId)> resolve =
            context.Resolve ? context.Resolve
                            : function<optional<ResolvedSource>(AssetId)>(
                                  [](AssetId) -> optional<ResolvedSource> { return std::nullopt; });

        // --- 2. Cook each entity's components ---

        vector<CookedPrefabEntity> entityTable;
        vector<CookedPrefabComponent> componentTable;
        vector<u8> records;
        entityTable.reserve(entityCount);

        for (usize entityIndex = 0; entityIndex < entityCount; ++entityIndex)
        {
            const json& entityJson = entities[entityIndex];
            if (!entityJson.is_object())
            {
                return std::unexpected(fmt::format(
                    "prefab importer: '{}': entity[{}] is not an object", file, entityIndex));
            }

            // The optional "id" key is the editor's stable per-entity round-trip identity: it
            // aligns a live entity to its source object across add / delete / reorder when the
            // editor saves back. It must be an unsigned integer when present; the cook validates
            // it but does not encode it, since entity identity in the cooked blob is positional
            // (a Reference field cooks to the entity's index in this array). Absent → the editor
            // falls back to positional order, so a hand-authored source with no ids still cooks.
            if (entityJson.contains("id") && !entityJson["id"].is_number_unsigned())
            {
                return std::unexpected(fmt::format(
                    "prefab importer: '{}': entity[{}] 'id' must be an unsigned integer", file,
                    entityIndex));
            }

            // Best-effort display name for diagnostics: the entity's Name component value
            // if it carries one. The entity[index] locator is always present in the
            // message, so an unnamed entity is still unambiguously identified.
            string entityName = "<unnamed>";
            if (entityJson.contains("components") && entityJson["components"].is_object())
            {
                const json& comps = entityJson["components"];
                const auto nameIt = comps.find("::Veng::Name");
                if (nameIt != comps.end() && nameIt->is_object())
                {
                    const auto valueIt = nameIt->find("Value");
                    if (valueIt != nameIt->end() && valueIt->is_string())
                    {
                        entityName = valueIt->get<string>();
                    }
                }
            }

            CookedPrefabEntity cookedEntity{};
            cookedEntity.FirstComponent = static_cast<u32>(componentTable.size());
            cookedEntity.ComponentCount = 0;

            if (entityJson.contains("components"))
            {
                if (!entityJson["components"].is_object())
                {
                    return std::unexpected(fmt::format(
                        "prefab importer: '{}': entity[{}] '{}': 'components' must be an object",
                        file, entityIndex, entityName));
                }

                const json& components = entityJson["components"];
                for (auto it = components.begin(); it != components.end(); ++it)
                {
                    const string& key = it.key();
                    const json& fieldsJson = it.value();

                    // --- 2a. Resolve the component key to a TypeId ---
                    // A registered type name, or a decimal TypeId for the keyless
                    // case.
                    TypeId typeId = InvalidTypeId;
                    string typeName = key;
                    bool found = false;
                    for (const auto& [id, info] : registry.All())
                    {
                        if (TypeNameMatches(info, key))
                        {
                            typeId = id;
                            found = true;
                            break;
                        }
                    }
                    if (!found)
                    {
                        // Try the key as a decimal TypeId.
                        const u64 parsed = std::strtoull(key.c_str(), nullptr, 10);
                        if (parsed != 0 && registry.IsRegistered(parsed))
                        {
                            typeId = parsed;
                            typeName = registry.Info(parsed).Name;
                            found = true;
                        }
                    }
                    if (!found)
                    {
                        return std::unexpected(fmt::format(
                            "prefab importer: '{}': entity[{}] '{}': unknown component '{}'", file,
                            entityIndex, entityName, key));
                    }

                    const TypeInfo& typeInfo = registry.Info(typeId);
                    if (typeName.empty())
                    {
                        typeName = typeInfo.Name;
                    }

                    if (!fieldsJson.is_object())
                    {
                        return std::unexpected(
                            fmt::format("prefab importer: '{}': entity[{}] '{}' component '{}': "
                                        "value must be an object of fields",
                                        file, entityIndex, entityName, typeName));
                    }

                    // --- 2b. Default-construct a type-erased instance ---
                    vector<u8> instance(typeInfo.Size);
                    typeInfo.DefaultConstruct(instance.data());

                    // --- 2c. Bind each JSON field through the shared walker, validating it ---
                    const JsonFieldHooks hooks = MakeHooks(entityCount, resolve);
                    VoidResult bindResult =
                        JsonReadFields(instance.data(), typeInfo, fieldsJson, registry, hooks);
                    if (!bindResult)
                    {
                        bindResult = std::unexpected(
                            Located(file, entityIndex, entityName, typeName, bindResult.error()));
                    }

                    // --- 2d. Serialize via WriteFields, destruct the instance ---
                    if (bindResult)
                    {
                        CookedPrefabComponent cookedComponent{};
                        cookedComponent.TypeId = typeId;
                        cookedComponent.RecordOffset = static_cast<u32>(records.size());
                        WriteFields(records, instance.data(), typeInfo, registry);
                        cookedComponent.RecordSize =
                            static_cast<u32>(records.size()) - cookedComponent.RecordOffset;
                        componentTable.push_back(cookedComponent);
                        ++cookedEntity.ComponentCount;
                    }

                    typeInfo.Destruct(instance.data());

                    if (!bindResult)
                    {
                        return std::unexpected(bindResult.error());
                    }
                }
            }

            entityTable.push_back(cookedEntity);
        }

        // --- 3. Assemble the blob ---

        CookedPrefabHeader header{};
        header.Version = CookedPrefabVersion;
        header.EntityCount = static_cast<u32>(entityTable.size());
        header.ComponentCount = static_cast<u32>(componentTable.size());
        header.RecordBytes = static_cast<u32>(records.size());

        vector<u8> blob;
        blob.reserve(sizeof(CookedPrefabHeader) + entityTable.size() * sizeof(CookedPrefabEntity) +
                     componentTable.size() * sizeof(CookedPrefabComponent) + records.size());

        Append(blob, header);
        for (const CookedPrefabEntity& e : entityTable)
        {
            Append(blob, e);
        }
        for (const CookedPrefabComponent& c : componentTable)
        {
            Append(blob, c);
        }
        blob.insert(blob.end(), records.begin(), records.end());

        return blob;
    }

    void RegisterPrefabImporter(Cooker& cooker)
    {
        cooker.Register(CreateUnique<PrefabImporter>());
    }
}
