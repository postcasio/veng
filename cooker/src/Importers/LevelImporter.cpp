#include "LevelImporter.h"

#include <Veng/Cook/BuiltinImporters.h>
#include <Veng/Cook/Cooker.h>

#include <cstring>
#include <fstream>
#include <sstream>

#include <fmt/format.h>

#include <Veng/Asset/CookedBlobs.h>
#include <Veng/Asset/HexId.h>
#include <Veng/Asset/Prefab.h>
#include <Veng/Cook/JsonFile.h>
#include <Veng/Reflection/JsonSerialize.h>
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
        string Located(const string& file, const string& section, const string& reason)
        {
            return fmt::format("level importer: '{}': {}: {}", file, section, reason);
        }

        // Binds a JSON config object into a default-constructed instance of `type` through the
        // shared walker, then serializes the instance via WriteFields into `out`. Omitted fields
        // keep their default (schema tolerance); an unknown or malformed field is a located error.
        // Reference stays unset, so a Reference field in level config is a located error — the
        // level config records (game mode, render settings) name no entity.
        Result<vector<u8>> CookConfigRecord(const json& configJson, const TypeInfo& type,
                                            const TypeRegistry& registry, const string& file,
                                            const string& section)
        {
            vector<u8> instance(type.Size);
            type.DefaultConstruct(instance.data());

            VoidResult bindResult = JsonReadFields(instance.data(), type, configJson, registry);
            if (!bindResult)
            {
                bindResult = std::unexpected(Located(file, section, bindResult.error()));
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

        const Result<json> levelResult = ReadJsonFile(levelPath, "level importer");
        if (!levelResult)
        {
            return std::unexpected(levelResult.error());
        }
        const json& level = *levelResult;

        // --- 2. The world prefab reference ---

        if (!level.contains("world") || !level["world"].is_string())
        {
            return std::unexpected(fmt::format("level importer: '{}': missing or invalid 'world' "
                                               "prefab id (expected hex id string)",
                                               file));
        }
        const optional<AssetId> worldParsed = ParseAssetId(level["world"].get<string>());
        if (!worldParsed)
        {
            return std::unexpected(
                fmt::format("level importer: '{}': 'world' is a malformed hex id '{}'", file,
                            level["world"].get<string>()));
        }
        const u64 worldId = worldParsed->Value;
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
                if (!idValue.is_string())
                {
                    return std::unexpected(fmt::format(
                        "level importer: '{}': 'systems' entry must be a hex id string", file));
                }
                const optional<u64> sysParsed = ParseHexId(idValue.get<string>());
                if (!sysParsed)
                {
                    return std::unexpected(fmt::format(
                        "level importer: '{}': 'systems' entry is a malformed hex id '{}'", file,
                        idValue.get<string>()));
                }
                const u64 sysId = *sysParsed;

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
