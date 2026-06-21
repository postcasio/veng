#include "PrefabImporter.h"

#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>

#include <cstring>
#include <fstream>
#include <sstream>

#include <fmt/format.h>

#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/Mesh.h>
#include <Veng/Asset/Material.h>
#include <Veng/Asset/Texture.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Scene/Entity.h>

namespace Veng::Cook
{
    namespace
    {
        // The AssetType an AssetHandle<T> field expects, keyed by the field's leaf
        // TypeId (the stable ids on VengReflect<AssetHandle<T>>). A field whose
        // type is none of these is an AssetHandle of an unknown asset type.
        optional<AssetType> AssetTypeForHandleField(TypeId fieldType)
        {
            if (fieldType == TypeIdOf<AssetHandle<Texture>>())
            {
                return AssetType::Texture;
            }
            if (fieldType == TypeIdOf<AssetHandle<Mesh>>())
            {
                return AssetType::Mesh;
            }
            if (fieldType == TypeIdOf<AssetHandle<Material>>())
            {
                return AssetType::Material;
            }
            return std::nullopt;
        }

        // Finds a field of `info` by name, or nullptr if none matches.
        const FieldDescriptor* FindField(const TypeInfo& info, const string& name)
        {
            for (const FieldDescriptor& field : info.Fields)
            {
                if (field.Name == name)
                {
                    return &field;
                }
            }
            return nullptr;
        }

        // Matches `name` against the registered TypeInfo.Name of each of the
        // variant's alternatives, returning the matched TypeId or InvalidTypeId.
        TypeId MatchAlternativeByName(const TypeInfo& variant, const string& name,
                                      const TypeRegistry& registry)
        {
            for (const TypeId altId : variant.VariantAlternatives)
            {
                if (registry.Info(altId).Name == name)
                {
                    return altId;
                }
            }
            return InvalidTypeId;
        }

        // Located-error prefix for a field within an entity's component.
        string Located(const string& file, usize entityIndex, const string& entityName,
                       const string& typeName, const string& field, const string& reason)
        {
            return fmt::format(
                "prefab importer: '{}': entity[{}] '{}' component '{}': field '{}': {}", file,
                entityIndex, entityName, typeName, field, reason);
        }

        // Binds one JSON value into the field at obj+field.Offset, validating it
        // against the field's FieldClass / leaf TypeId. Returns a located error on
        // any mismatch. entityCount is the prefab's entity count, for Reference
        // range checks; resolve looks AssetHandle ids up in the pack.
        VoidResult BindField(void* obj, const FieldDescriptor& field, const json& value,
                             const TypeRegistry& registry, usize entityCount,
                             const function<optional<ResolvedSource>(AssetId)>& resolve,
                             const string& file, usize entityIndex, const string& entityName,
                             const string& typeName)
        {
            auto err = [&](const string& reason)
            {
                return std::unexpected(
                    Located(file, entityIndex, entityName, typeName, field.Name, reason));
            };

            void* fieldPtr = static_cast<u8*>(obj) + field.Offset;

            switch (field.Class)
            {
            case FieldClass::Scalar:
            {
                if (!value.is_number() && !value.is_boolean())
                {
                    return err("expected a number or boolean");
                }

                // Scalars are bool/f32/i32/u32/u64; coerce to the field's
                // exact byte width via its leaf TypeId.
                const TypeId t = field.Type;
                if (t == TypeIdOf<bool>())
                {
                    if (!value.is_boolean() && !value.is_number())
                    {
                        return err("expected a boolean");
                    }
                    const bool v =
                        value.is_boolean() ? value.get<bool>() : (value.get<f64>() != 0.0);
                    std::memcpy(fieldPtr, &v, sizeof(v));
                }
                else if (t == TypeIdOf<f32>())
                {
                    const f32 v = value.get<f32>();
                    std::memcpy(fieldPtr, &v, sizeof(v));
                }
                else if (t == TypeIdOf<i32>())
                {
                    const i32 v = value.get<i32>();
                    std::memcpy(fieldPtr, &v, sizeof(v));
                }
                else if (t == TypeIdOf<u32>())
                {
                    const u32 v = value.get<u32>();
                    std::memcpy(fieldPtr, &v, sizeof(v));
                }
                else if (t == TypeIdOf<u64>())
                {
                    const u64 v = value.get<u64>();
                    std::memcpy(fieldPtr, &v, sizeof(v));
                }
                else
                {
                    return err("unsupported scalar leaf type");
                }
                return {};
            }

            case FieldClass::Vector:
            case FieldClass::Quaternion:
            case FieldClass::Matrix:
            {
                // Components are written in the field's storage type — f32 for a
                // float vector/quat/matrix, u32 for an unsigned-integer vector. A
                // quat is [x,y,z,w] (glm memory layout), so identity is [0,0,0,1].
                const bool unsignedVector = field.Type == TypeIdOf<uvec2>();
                const usize componentSize = unsignedVector ? sizeof(u32) : sizeof(f32);
                const usize size = registry.Info(field.Type).Size;
                const usize arity = size / componentSize;

                if (!value.is_array() || value.size() != arity)
                {
                    return err(fmt::format("expected an array of {} numbers", arity));
                }

                for (const json& elem : value)
                {
                    if (!elem.is_number())
                    {
                        return err("array contains a non-number element");
                    }
                }

                if (unsignedVector)
                {
                    vector<u32> components;
                    components.reserve(arity);
                    for (const json& elem : value)
                    {
                        components.push_back(elem.get<u32>());
                    }
                    std::memcpy(fieldPtr, components.data(), arity * sizeof(u32));
                }
                else
                {
                    vector<f32> components;
                    components.reserve(arity);
                    for (const json& elem : value)
                    {
                        components.push_back(elem.get<f32>());
                    }
                    std::memcpy(fieldPtr, components.data(), arity * sizeof(f32));
                }
                return {};
            }

            case FieldClass::String:
            {
                if (!value.is_string())
                {
                    return err("expected a string");
                }
                *static_cast<string*>(fieldPtr) = value.get<string>();
                return {};
            }

            case FieldClass::AssetHandle:
            {
                if (!value.is_number_unsigned())
                {
                    return err("expected an unsigned integer AssetId");
                }

                const u64 id = value.get<u64>();

                // An invalid (0) id is the "no asset" value — write it through.
                if (id != 0)
                {
                    const optional<AssetType> expected = AssetTypeForHandleField(field.Type);
                    const optional<ResolvedSource> resolved = resolve(AssetId{.Value = id});
                    // Resolve only validates ids present in this pack (or a
                    // --reference pack); a non-resident id is accepted as-is
                    // (residency is the runtime's job).
                    if (resolved && expected && resolved->Type != *expected)
                    {
                        return err(fmt::format(
                            "asset {} resolves to type {} but the field expects type {}", id,
                            static_cast<u32>(resolved->Type), static_cast<u32>(*expected)));
                    }
                }

                // The handle stores the AssetId at offset 0 (pinned in
                // AssetHandle.h); write the raw id there.
                std::memcpy(fieldPtr, &id, sizeof(id));
                return {};
            }

            case FieldClass::Enum:
            {
                if (!value.is_number_integer())
                {
                    return err("expected an integer enum value");
                }

                const usize size = registry.Info(field.Type).Size;
                const i64 raw = value.get<i64>();
                // Write the low `size` bytes of the integer (host little-endian).
                u64 bits = static_cast<u64>(raw);
                std::memcpy(fieldPtr, &bits, size);
                return {};
            }

            case FieldClass::Reference:
            {
                Entity& entity = *static_cast<Entity*>(fieldPtr);

                // A null reference (JSON null) stays Entity::Null.
                if (value.is_null())
                {
                    entity = Entity::Null;
                    return {};
                }

                if (!value.is_number_unsigned())
                {
                    return err("expected an unsigned entity index or null");
                }

                const u64 index = value.get<u64>();
                if (index >= entityCount)
                {
                    return err(fmt::format(
                        "entity reference index {} is out of range (prefab has {} entities)", index,
                        entityCount));
                }

                // The cooked reference stores the prefab-local index in Index,
                // Generation 0; the loader remaps it to the spawned handle.
                entity.Index = static_cast<u32>(index);
                entity.Generation = 0;
                return {};
            }

            case FieldClass::Struct:
            {
                if (!value.is_object())
                {
                    return err("expected an object");
                }

                const TypeInfo& nested = registry.Info(field.Type);
                for (auto it = value.begin(); it != value.end(); ++it)
                {
                    const FieldDescriptor* match = nullptr;
                    for (const FieldDescriptor& nestedField : nested.Fields)
                    {
                        if (nestedField.Name == it.key())
                        {
                            match = &nestedField;
                            break;
                        }
                    }
                    if (match == nullptr)
                    {
                        return err(fmt::format("nested field '{}' is not in type '{}'", it.key(),
                                               nested.Name));
                    }

                    const VoidResult bound =
                        BindField(fieldPtr, *match, it.value(), registry, entityCount, resolve,
                                  file, entityIndex, entityName, typeName);
                    if (!bound)
                    {
                        return bound;
                    }
                }
                return {};
            }

            case FieldClass::Variant:
            {
                // A variant is authored as { "type": <registered name>, "value": {…} }.
                // The cooked bytes carry the active TypeId tag; the shared WriteFields
                // (not this binder) emits them, so binding only selects the active
                // alternative on the live instance and recurses into its fields.
                if (!value.is_object() || !value.contains("type") || !value["type"].is_string())
                {
                    return err("expected an object with a string 'type' key");
                }

                const TypeInfo& info = registry.Info(field.Type);
                const string typeName = value["type"].get<string>();

                // The empty selection: leave the default-constructed (empty) variant,
                // which WriteFields emits as the InvalidTypeId tag.
                if (typeName.empty())
                {
                    return {};
                }

                const TypeId chosen = MatchAlternativeByName(info, typeName, registry);
                if (chosen == InvalidTypeId)
                {
                    return err(fmt::format("'{}' is not an alternative of variant '{}'", typeName,
                                           info.Name));
                }

                void* memberPtr = info.VariantSetActive(fieldPtr, chosen);
                // chosen came from this variant's alternative list, so SetActive succeeds.

                if (value.contains("value"))
                {
                    const json& inner = value["value"];
                    if (!inner.is_object())
                    {
                        return err("variant 'value' must be an object");
                    }

                    const TypeInfo& alt = registry.Info(chosen);
                    for (auto it = inner.begin(); it != inner.end(); ++it)
                    {
                        const FieldDescriptor* match = FindField(alt, it.key());
                        if (match == nullptr)
                        {
                            return err(fmt::format("nested field '{}' is not in alternative '{}'",
                                                   it.key(), alt.Name));
                        }

                        const VoidResult bound =
                            BindField(memberPtr, *match, it.value(), registry, entityCount, resolve,
                                      file, entityIndex, entityName, typeName);
                        if (!bound)
                        {
                            return bound;
                        }
                    }
                }
                return {};
            }
            }

            return err("unhandled field class");
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

        const std::ifstream prefabFile(prefabPath, std::ios::binary);
        if (!prefabFile)
        {
            return std::unexpected(fmt::format("prefab importer: failed to open '{}'", file));
        }

        std::ostringstream prefabStream;
        prefabStream << prefabFile.rdbuf();
        const json prefab = json::parse(prefabStream.str(), nullptr, false);
        if (prefab.is_discarded() || !prefab.is_object())
        {
            return std::unexpected(fmt::format("prefab importer: '{}': invalid JSON", file));
        }

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
                        if (!info.Name.empty() && info.Name == key)
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

                    // --- 2c. Bind each JSON field, validating it ---
                    VoidResult bindResult{};
                    for (auto fieldIt = fieldsJson.begin(); fieldIt != fieldsJson.end(); ++fieldIt)
                    {
                        const FieldDescriptor* match = nullptr;
                        for (const FieldDescriptor& f : typeInfo.Fields)
                        {
                            if (f.Name == fieldIt.key())
                            {
                                match = &f;
                                break;
                            }
                        }
                        if (match == nullptr)
                        {
                            bindResult = std::unexpected(
                                Located(file, entityIndex, entityName, typeName, fieldIt.key(),
                                        "field is not in the component's descriptor"));
                            break;
                        }

                        bindResult = BindField(instance.data(), *match, fieldIt.value(), registry,
                                               entityCount, resolve, file, entityIndex, entityName,
                                               typeName);
                        if (!bindResult)
                        {
                            break;
                        }
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
