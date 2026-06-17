#include "AssetSourceIndex.h"

#include <Veng/Log.h>

#include <fstream>
#include <sstream>

#include <nlohmann/json.hpp>

namespace VengEditor
{
    using namespace Veng;

    namespace
    {
        // The manifest "type" strings the cooker recognizes (Cooker.h schema).
        optional<AssetType> ParseAssetType(const std::string& name)
        {
            if (name == "raw") return AssetType::Raw;
            if (name == "texture") return AssetType::Texture;
            if (name == "mesh") return AssetType::Mesh;
            if (name == "shader") return AssetType::Shader;
            if (name == "material") return AssetType::Material;
            if (name == "vertex_layout") return AssetType::VertexLayout;
            if (name == "prefab") return AssetType::Prefab;
            return std::nullopt;
        }
    }

    AssetSourceIndex AssetSourceIndex::Parse(const path& manifestPath)
    {
        AssetSourceIndex index;

        std::ifstream file(manifestPath, std::ios::binary);
        if (!file)
        {
            Log::Error("AssetSourceIndex: failed to open manifest {}", manifestPath.string());
            return index;
        }

        std::ostringstream contents;
        contents << file.rdbuf();
        const nlohmann::json manifest = nlohmann::json::parse(contents.str(), nullptr, false);
        if (manifest.is_discarded() || !manifest.is_object() || !manifest.contains("assets") ||
            !manifest["assets"].is_array())
        {
            Log::Error("AssetSourceIndex: malformed manifest {}", manifestPath.string());
            return index;
        }

        const path manifestDir = manifestPath.parent_path();
        for (const nlohmann::json& entry : manifest["assets"])
        {
            if (!entry.is_object() || !entry.contains("id") || !entry["id"].is_number_unsigned() ||
                !entry.contains("type") || !entry["type"].is_string() ||
                !entry.contains("source") || !entry["source"].is_string())
                continue;

            const optional<AssetType> type = ParseAssetType(entry["type"].get<std::string>());
            if (!type)
                continue;

            const u64 id = entry["id"].get<u64>();
            index.m_Entries[id] = Entry{
                .Type = *type,
                .Source = manifestDir / entry["source"].get<std::string>(),
            };
        }

        return index;
    }

    const AssetSourceIndex::Entry* AssetSourceIndex::Find(AssetId id) const
    {
        const auto it = m_Entries.find(id.Value);
        return it == m_Entries.end() ? nullptr : &it->second;
    }

    vector<AssetId> AssetSourceIndex::EntriesOfType(AssetType type) const
    {
        vector<AssetId> ids;
        for (const auto& [id, entry] : m_Entries)
            if (entry.Type == type)
                ids.push_back(AssetId{id});
        return ids;
    }
}
