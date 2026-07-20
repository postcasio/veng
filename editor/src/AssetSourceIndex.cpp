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
                // The asset is dropped from the index, so it degrades across the browser, every
                // picker, and the MCP asset tools with no shared symptom — say so once, here.
                Log::Error("AssetSourceIndex: manifest '{}': unknown asset type '{}'; the asset is "
                           "omitted from the index",
                           manifestPath.string(), entry["type"].get<std::string>());
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

            // A material may name a companion default MaterialInstance: the cook emits a
            // zero-override instance at that id, and every direct material reference resolves it.
            // It has no source file of its own, so register a synthesized entry — named
            // "<material> (default)" beside the parent — so the browser shows it named and in-folder
            // rather than as a loose, id-only asset at the root.
            if (*type == AssetTypes::Material)
            {
                const optional<nlohmann::json> material = ReadJsonObject(manifestDir / source);
                if (material && material->contains("defaultInstance") &&
                    (*material)["defaultInstance"].is_string())
                {
                    const optional<AssetId> defaultId =
                        ParseAssetId((*material)["defaultInstance"].get<std::string>());
                    if (defaultId && !index.m_Entries.contains(defaultId->Value))
                    {
                        const path parentRel{source};
                        path base = parentRel.filename().stem();
                        if (base.has_extension())
                        {
                            base = base.stem();
                        }
                        index.m_Entries[defaultId->Value] = Entry{
                            .Type = AssetTypes::MaterialInstance,
                            .Source = {},
                            .RelativeSource = parentRel.parent_path() /
                                              (base.string() + " (default).vmatinst.json"),
                            .Synthesized = true,
                        };
                    }
                }
            }
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
