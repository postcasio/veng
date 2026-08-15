#include "PrefabImporter.h"

#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>

#include <algorithm>
#include <cstring>
#include <fstream>
#include <sstream>

#include <fmt/format.h>

#include <Veng/Asset/AssetHandleType.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/HexId.h>
#include <Veng/Cook/JsonFile.h>
#include <Veng/Reflection/JsonSerialize.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Scene/Entity.h>

namespace Veng::Cook
{
    namespace
    {
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
                                 const function<optional<ResolvedSource>(AssetId)>& resolve,
                                 const AssetTypeRegistry& assetTypes)
        {
            JsonFieldHooks hooks;

            hooks.ValidateAssetId = [resolve, &assetTypes](u64 id, TypeId fieldType) -> VoidResult
            {
                const optional<AssetTypeId> expected = assetTypes.FindByHandleField(fieldType);
                if (!expected)
                {
                    // No registered mapping means the field's asset type never declared its
                    // handle leaf — a registration mistake. Skipping the check instead would
                    // let the field accept an id of any type at all.
                    return std::unexpected(fmt::format(
                        "field is an AssetHandle whose leaf type {} no registered asset type "
                        "claims; set HandleFieldType on the type's registration (and pass "
                        "--module if it is a module-defined type)",
                        FormatHexId(fieldType)));
                }

                const optional<ResolvedSource> resolved = resolve(AssetId{.Value = id});
                // Resolve only validates ids present in this pack (or a --reference pack);
                // a non-resident id is accepted as-is (residency is the runtime's job).
                if (resolved && !AssetHandleFieldAccepts(*expected, resolved->Type))
                {
                    return std::unexpected(fmt::format(
                        "asset {} resolves to type {} but the field expects type {}", id,
                        assetTypes.GetName(resolved->Type), assetTypes.GetName(*expected)));
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

        // A resolved nesting edge: the prefab an entity names as its body, and where its source
        // lives. The invalid id means the entity named none.
        struct NestedRef
        {
            AssetId Id;
            path Source;
        };

        // Reads an entity's optional "prefab" key and resolves it to that prefab's source file.
        // A malformed, unresolvable, or wrong-typed id is a located error.
        Result<NestedRef> ReadNestedRef(const string& file, usize entityIndex,
                                        const json& entityJson,
                                        const function<optional<ResolvedSource>(AssetId)>& resolve,
                                        const AssetTypeRegistry& assetTypes)
        {
            if (!entityJson.contains("prefab"))
            {
                return NestedRef{};
            }

            const json& value = entityJson["prefab"];
            if (!value.is_string())
            {
                return std::unexpected(
                    fmt::format("prefab importer: '{}': entity[{}] 'prefab' must be a hex id "
                                "string naming the prefab that is this entity's body",
                                file, entityIndex));
            }

            const optional<AssetId> parsed = ParseAssetId(value.get<string>());
            if (!parsed || !parsed->IsValid())
            {
                return std::unexpected(
                    fmt::format("prefab importer: '{}': entity[{}] 'prefab' is a malformed or "
                                "zero hex id '{}'",
                                file, entityIndex, value.get<string>()));
            }

            // A nesting edge must resolve: the cook walks it to reject a cycle, which it cannot
            // do for an id whose source it cannot reach. That is stricter than the AssetHandle
            // policy beside it, where residency is the runtime's job.
            const optional<ResolvedSource> resolved = resolve(*parsed);
            if (!resolved)
            {
                return std::unexpected(fmt::format(
                    "prefab importer: '{}': entity[{}] 'prefab' names asset {} which this cook "
                    "cannot resolve — add it to the pack, or pass its pack with --reference",
                    file, entityIndex, FormatAssetId(*parsed)));
            }
            if (resolved->Type != AssetTypes::Prefab)
            {
                return std::unexpected(fmt::format(
                    "prefab importer: '{}': entity[{}] 'prefab' names asset {} which resolves to "
                    "type {}, not a prefab",
                    file, entityIndex, FormatAssetId(*parsed), assetTypes.GetName(resolved->Type)));
            }

            return NestedRef{.Id = *parsed, .Source = resolved->AbsolutePath};
        }

        // Depth-first walk of the nesting edges reachable from one prefab source, failing on a
        // prefab that transitively names itself — which would recurse forever at spawn. `chain`
        // is the path from the cooked prefab down to `source`, and names the cycle in the error.
        VoidResult CheckNoNestingCycle(const path& source, const json& document,
                                       vector<path>& chain, vector<path>& acyclic,
                                       const function<optional<ResolvedSource>(AssetId)>& resolve,
                                       const AssetTypeRegistry& assetTypes,
                                       const function<void(const path&)>& recordDependency)
        {
            if (!document.contains("entities") || !document["entities"].is_array())
            {
                return {};
            }

            chain.push_back(source);
            for (usize i = 0; i < document["entities"].size(); ++i)
            {
                const json& entityJson = document["entities"][i];
                if (!entityJson.is_object())
                {
                    continue;
                }

                const Result<NestedRef> nested =
                    ReadNestedRef(source.string(), i, entityJson, resolve, assetTypes);
                if (!nested)
                {
                    return std::unexpected(nested.error());
                }
                if (!nested->Id.IsValid())
                {
                    continue;
                }

                if (std::ranges::find(chain, nested->Source) != chain.end())
                {
                    string cycle;
                    for (const path& step : chain)
                    {
                        cycle += step.string();
                        cycle += " -> ";
                    }
                    cycle += nested->Source.string();
                    return std::unexpected(
                        fmt::format("prefab importer: '{}': entity[{}] nests a prefab that "
                                    "transitively contains it: {}",
                                    source.string(), i, cycle));
                }

                // A prefab reached twice through different branches is walked once: the chain
                // above already proved it reaches no ancestor of its own.
                if (std::ranges::find(acyclic, nested->Source) != acyclic.end())
                {
                    continue;
                }

                // The child's own source decides whether this prefab is part of a cycle, so a
                // change to it must re-run this cook even though its bytes never enter the blob.
                if (recordDependency)
                {
                    recordDependency(nested->Source);
                }

                const Result<json> child = ReadJsonFile(nested->Source, "prefab importer");
                if (!child)
                {
                    return std::unexpected(child.error());
                }

                const VoidResult descended = CheckNoNestingCycle(
                    nested->Source, *child, chain, acyclic, resolve, assetTypes, recordDependency);
                if (!descended)
                {
                    return descended;
                }
                acyclic.push_back(nested->Source);
            }
            chain.pop_back();

            return {};
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

        // A prefab that transitively names itself would recurse forever at spawn, so the nesting
        // edges are walked before anything is encoded.
        {
            vector<path> chain;
            vector<path> acyclic;
            const VoidResult acyclicResult =
                CheckNoNestingCycle(prefabPath, prefab, chain, acyclic, resolve,
                                    *context.AssetTypes, context.RecordDependency);
            if (!acyclicResult)
            {
                return std::unexpected(acyclicResult.error());
            }
        }

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

            // The optional "prefab" key names the prefab that is this entity's body; the
            // entity's own components then read as overrides over that expansion.
            const Result<NestedRef> nested =
                ReadNestedRef(file, entityIndex, entityJson, resolve, *context.AssetTypes);
            if (!nested)
            {
                return std::unexpected(nested.error());
            }

            CookedPrefabEntity cookedEntity{};
            cookedEntity.FirstComponent = static_cast<u32>(componentTable.size());
            cookedEntity.ComponentCount = 0;
            cookedEntity.NestedPrefab = nested->Id.Value;

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
                    const JsonFieldHooks hooks =
                        MakeHooks(entityCount, resolve, *context.AssetTypes);
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
