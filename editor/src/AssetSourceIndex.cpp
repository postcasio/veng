#include "AssetSourceIndex.h"

#include "JsonUtil.h"

#include <Veng/Asset/HexId.h>
#include <Veng/Log.h>

#include <fstream>
#include <span>
#include <sstream>

#include <nlohmann/json.hpp>

namespace VengEditor
{
    using namespace Veng;

    AssetSourceIndex AssetSourceIndex::Parse(const path& manifestPath,
                                             const AssetTypeRegistry& types)
    {
        AssetSourceIndex index;
        index.m_Types = &types;

        const optional<nlohmann::json> manifestResult = ReadJsonObject(manifestPath);
        if (!manifestResult)
        {
            Log::Error("AssetSourceIndex: failed to read manifest {}", manifestPath.string());
            return index;
        }
        const nlohmann::json& manifest = *manifestResult;
        if (!manifest.contains("assets") || !manifest["assets"].is_array())
        {
            Log::Error("AssetSourceIndex: malformed manifest {}", manifestPath.string());
            return index;
        }

        const path manifestDir = manifestPath.parent_path();
        for (const nlohmann::json& entry : manifest["assets"])
        {
            if (!entry.is_object() || !entry.contains("id") || !entry["id"].is_string() ||
                !entry.contains("type") || !entry["type"].is_string() ||
                !entry.contains("source") || !entry["source"].is_string())
            {
                continue;
            }

            const optional<AssetTypeId> type = types.FindByName(entry["type"].get<std::string>());
            if (!type)
            {
                continue;
            }

            const optional<AssetId> parsedId = ParseAssetId(entry["id"].get<std::string>());
            if (!parsedId)
            {
                Log::Error("AssetSourceIndex: manifest '{}': malformed hex id '{}'",
                           manifestPath.string(), entry["id"].get<std::string>());
                continue;
            }
            const u64 id = parsedId->Value;
            const std::string source = entry["source"].get<std::string>();
            index.m_Entries[id] = Entry{
                .Type = *type,
                .Source = manifestDir / source,
                .RelativeSource = path{source},
            };
        }

        return index;
    }

    AssetSourceIndex AssetSourceIndex::ParsePacks(std::span<const path> manifestPaths,
                                                  const AssetTypeRegistry& types)
    {
        AssetSourceIndex index;
        index.m_Types = &types;
        for (const path& manifestPath : manifestPaths)
        {
            const AssetSourceIndex one = Parse(manifestPath, types);
            for (const auto& [id, entry] : one.m_Entries)
            {
                index.m_Entries[id] = entry;
            }
        }
        return index;
    }

    const AssetSourceIndex::Entry* AssetSourceIndex::Find(AssetId id) const
    {
        const auto it = m_Entries.find(id.Value);
        return it == m_Entries.end() ? nullptr : &it->second;
    }

    vector<AssetId> AssetSourceIndex::EntriesOfType(AssetTypeId type) const
    {
        vector<AssetId> ids;
        for (const auto& [id, entry] : m_Entries)
        {
            if (entry.Type == type)
            {
                ids.push_back(AssetId{id});
            }
        }
        return ids;
    }

    void AssetSourceIndex::ForEachEntry(const function<void(AssetId, const Entry&)>& fn) const
    {
        for (const auto& [id, entry] : m_Entries)
        {
            fn(AssetId{id}, entry);
        }
    }
}
