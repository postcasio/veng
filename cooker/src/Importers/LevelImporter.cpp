#include "LevelImporter.h"

#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>

#include <cstring>
#include <fstream>
#include <sstream>

#include <fmt/format.h>

#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/Prefab.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeId.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Components.h>
#include <Veng/Scene/SystemRegistry.h>

namespace Veng::Cook
{
    namespace
    {
        // Located-error prefix for a level config field.
        string Located(const string& file, const string& section, const string& field,
                       const string& reason)
        {
            return fmt::format("level importer: '{}': {}: field '{}': {}", file, section, field,
                               reason);
        }

        // Binds one JSON value into the field at obj+field.Offset, validating it against the
        // field's FieldClass / leaf TypeId. The level config structs are scalar/vector/handle
        // records (game mode, render settings), so this covers Scalar, Vector, AssetHandle, and a
        // nested Struct; an unsupported class is a located error rather than silent omission.
        VoidResult BindField(void* obj, const FieldDescriptor& field, const json& value,
                             const TypeRegistry& registry, const string& file,
                             const string& section)
        {
            auto err = [&](const string& reason)
            { return std::unexpected(Located(file, section, field.Name, reason)); };

            void* fieldPtr = static_cast<u8*>(obj) + field.Offset;

            switch (field.Class)
            {
            case FieldClass::Scalar:
            {
                if (!value.is_number() && !value.is_boolean())
                {
                    return err("expected a number or boolean");
                }

                const TypeId t = field.Type;
                if (t == TypeIdOf<bool>())
                {
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
            {
                // Components are written in the field's storage type — f32 for a float vector, u32
                // for an unsigned-integer vector — arity = byte size / component size.
                const bool unsignedVector = field.Type == TypeIdOf<uvec2>();
                const usize componentSize = unsignedVector ? sizeof(u32) : sizeof(f32);
                const usize arity = registry.Info(field.Type).Size / componentSize;

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

            case FieldClass::AssetHandle:
            {
                if (!value.is_number_unsigned())
                {
                    return err("expected an unsigned integer AssetId");
                }

                // The handle stores the AssetId at offset 0 (pinned in AssetHandle.h); write the
                // raw id there. Residency is the runtime's job — the loader rehydrates it.
                const u64 id = value.get<u64>();
                std::memcpy(fieldPtr, &id, sizeof(id));
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
                        BindField(fieldPtr, *match, it.value(), registry, file, section);
                    if (!bound)
                    {
                        return bound;
                    }
                }
                return {};
            }

            default:
                return err("unsupported field class in level config");
            }
        }

        // Binds a JSON config object into a default-constructed instance of `type`, validating
        // each field, then serializes the instance via WriteFields into `out`. Omitted fields
        // keep their default (schema tolerance); an unknown or malformed field is a located error.
        Result<vector<u8>> CookConfigRecord(const json& configJson, const TypeInfo& type,
                                            const TypeRegistry& registry, const string& file,
                                            const string& section)
        {
            vector<u8> instance(type.Size);
            type.DefaultConstruct(instance.data());

            VoidResult bindResult{};
            for (auto it = configJson.begin(); it != configJson.end(); ++it)
            {
                const FieldDescriptor* match = nullptr;
                for (const FieldDescriptor& f : type.Fields)
                {
                    if (f.Name == it.key())
                    {
                        match = &f;
                        break;
                    }
                }
                if (match == nullptr)
                {
                    bindResult = std::unexpected(Located(
                        file, section, it.key(), "field is not in the config's descriptor"));
                    break;
                }
                bindResult =
                    BindField(instance.data(), *match, it.value(), registry, file, section);
                if (!bindResult)
                {
                    break;
                }
            }

            vector<u8> record;
            if (bindResult)
            {
                WriteFields(record, instance.data(), type, registry);
            }
            type.Destruct(instance.data());

            if (!bindResult)
            {
                return std::unexpected(bindResult.error());
            }
            return record;
        }

        template <class T>
        void Append(vector<u8>& out, const T& value)
        {
            const auto* p = reinterpret_cast<const u8*>(&value);
            out.insert(out.end(), p, p + sizeof(T));
        }
    }

    Result<vector<u8>> LevelImporter::Cook(const CookContext& context, const json& entry) const
    {
        // --- 0. The reflected registries (--module) are required ---

        if (context.Types == nullptr || context.Systems == nullptr)
        {
            return std::unexpected("level cooking requires --module");
        }

        const TypeRegistry& registry = *context.Types;
        const SystemRegistry& systems = *context.Systems;

        // --- 1. Read + parse the external *.level.json ---

        if (!entry.contains("source") || !entry["source"].is_string())
        {
            return std::unexpected("level importer: missing or invalid 'source'");
        }

        const path levelPath = context.PackDir / entry["source"].get<string>();
        const string file = levelPath.string();

        const std::ifstream levelFile(levelPath, std::ios::binary);
        if (!levelFile)
        {
            return std::unexpected(fmt::format("level importer: failed to open '{}'", file));
        }

        std::ostringstream levelStream;
        levelStream << levelFile.rdbuf();
        const json level = json::parse(levelStream.str(), nullptr, false);
        if (level.is_discarded() || !level.is_object())
        {
            return std::unexpected(fmt::format("level importer: '{}': invalid JSON", file));
        }

        // --- 2. The world prefab reference ---

        if (!level.contains("world") || !level["world"].is_number_unsigned())
        {
            return std::unexpected(
                fmt::format("level importer: '{}': missing or invalid 'world' prefab id", file));
        }
        const u64 worldId = level["world"].get<u64>();
        if (worldId == 0)
        {
            return std::unexpected(
                fmt::format("level importer: '{}': 'world' prefab id must be non-zero", file));
        }

        // A resolvable world id must point at a prefab; residency is the runtime's job, so a
        // non-resident id is accepted as-is (matching the prefab importer's handle policy).
        if (context.Resolve)
        {
            const optional<ResolvedSource> resolved = context.Resolve(AssetId{.Value = worldId});
            if (resolved && resolved->Type != AssetType::Prefab)
            {
                return std::unexpected(fmt::format(
                    "level importer: '{}': 'world' id {} resolves to type {}, not a prefab", file,
                    worldId, static_cast<u32>(resolved->Type)));
            }
        }

        // --- 3. The ordered system-id set ---

        vector<u64> systemIds;
        if (level.contains("systems"))
        {
            if (!level["systems"].is_array())
            {
                return std::unexpected(
                    fmt::format("level importer: '{}': 'systems' must be an array", file));
            }
            for (const json& idValue : level["systems"])
            {
                if (!idValue.is_number_unsigned())
                {
                    return std::unexpected(fmt::format(
                        "level importer: '{}': 'systems' entry must be an unsigned SystemId",
                        file));
                }
                const u64 sysId = idValue.get<u64>();

                // Each named system must resolve against the module's registered catalog.
                bool known = false;
                for (const SystemEntry& catalogEntry : systems.Entries())
                {
                    if (catalogEntry.Id == sysId)
                    {
                        known = true;
                        break;
                    }
                }
                if (!known)
                {
                    return std::unexpected(
                        fmt::format("level importer: '{}': system id {} is not registered by the "
                                    "module",
                                    file, sysId));
                }
                systemIds.push_back(sysId);
            }
        }

        // --- 4. The game-mode and render config records (tolerant reflection records) ---

        const json emptyObject = json::object();
        const json& gameModeJson = level.contains("gameMode") ? level["gameMode"] : emptyObject;
        if (!gameModeJson.is_object())
        {
            return std::unexpected(
                fmt::format("level importer: '{}': 'gameMode' must be an object", file));
        }
        const Result<vector<u8>> gameModeRecord = CookConfigRecord(
            gameModeJson, registry.Info(TypeIdOf<GameModeConfig>()), registry, file, "gameMode");
        if (!gameModeRecord)
        {
            return std::unexpected(gameModeRecord.error());
        }

        const json& renderJson = level.contains("render") ? level["render"] : emptyObject;
        if (!renderJson.is_object())
        {
            return std::unexpected(
                fmt::format("level importer: '{}': 'render' must be an object", file));
        }
        const Result<vector<u8>> renderRecord = CookConfigRecord(
            renderJson, registry.Info(TypeIdOf<LevelRenderSettings>()), registry, file, "render");
        if (!renderRecord)
        {
            return std::unexpected(renderRecord.error());
        }

        // --- 5. Assemble the blob ---

        CookedLevelHeader header{};
        header.Version = CookedLevelVersion;
        header.WorldPrefabId = worldId;
        header.SystemCount = static_cast<u32>(systemIds.size());
        header.GameModeRecordBytes = static_cast<u32>(gameModeRecord->size());
        header.RenderRecordBytes = static_cast<u32>(renderRecord->size());

        vector<u8> blob;
        blob.reserve(sizeof(CookedLevelHeader) + systemIds.size() * sizeof(u64) +
                     gameModeRecord->size() + renderRecord->size());

        Append(blob, header);
        for (const u64 sysId : systemIds)
        {
            Append(blob, sysId);
        }
        blob.insert(blob.end(), gameModeRecord->begin(), gameModeRecord->end());
        blob.insert(blob.end(), renderRecord->begin(), renderRecord->end());

        return blob;
    }

    void RegisterLevelImporter(Cooker& cooker)
    {
        cooker.Register(CreateUnique<LevelImporter>());
    }
}
