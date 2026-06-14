#include <Veng/Cook/Cooker.h>

#include <fstream>
#include <sstream>

#include <fmt/format.h>

namespace Veng::Cook
{
    namespace
    {
        optional<AssetType> ParseAssetType(const string& name)
        {
            if (name == "raw")
                return AssetType::Raw;
            if (name == "texture")
                return AssetType::Texture;
            if (name == "mesh")
                return AssetType::Mesh;
            if (name == "shader")
                return AssetType::Shader;
            if (name == "material")
                return AssetType::Material;

            return std::nullopt;
        }
    }

    void Cooker::Register(Unique<AssetImporter> importer)
    {
        const AssetType type = importer->Type();
        m_Importers[type] = std::move(importer);
    }

    VoidResult Cooker::CookPack(const path& packJson, const path& outArchive) const
    {
        std::ifstream file(packJson, std::ios::binary);
        if (!file)
            return std::unexpected(fmt::format("pack '{}': failed to open", packJson.string()));

        std::ostringstream contentStream;
        contentStream << file.rdbuf();
        const string content = contentStream.str();

        const json pack = json::parse(content, nullptr, false);
        if (pack.is_discarded())
            return std::unexpected(fmt::format("pack '{}': invalid JSON", packJson.string()));

        if (!pack.is_object() || !pack.contains("version") || !pack["version"].is_number_unsigned())
            return std::unexpected(fmt::format("pack '{}': missing or invalid 'version'", packJson.string()));

        const u64 version = pack["version"].get<u64>();
        if (version != 1)
        {
            return std::unexpected(fmt::format(
                "pack '{}': unsupported version {} (expected 1)", packJson.string(), version));
        }

        if (!pack.contains("assets") || !pack["assets"].is_array())
            return std::unexpected(fmt::format("pack '{}': missing 'assets' array", packJson.string()));

        const CookContext context{.PackDir = packJson.parent_path()};

        ArchiveWriter writer;
        std::set<u64> seenIds;

        const json& assets = pack["assets"];
        for (usize index = 0; index < assets.size(); ++index)
        {
            const VoidResult entryResult = CookEntry(context, assets[index], seenIds, writer);
            if (!entryResult)
            {
                return std::unexpected(fmt::format(
                    "pack '{}': asset[{}]: {}", packJson.string(), index, entryResult.error()));
            }
        }

        return writer.Write(outArchive);
    }

    VoidResult Cooker::CookEntry(const CookContext& context, const json& entry, std::set<u64>& seenIds, ArchiveWriter& writer) const
    {
        if (!entry.is_object())
            return std::unexpected("entry is not an object");

        if (!entry.contains("id") || !entry["id"].is_number_unsigned())
            return std::unexpected("missing or invalid 'id' (expected a non-zero u64)");

        const u64 id = entry["id"].get<u64>();
        if (id == 0)
            return std::unexpected("asset id 0 is reserved (invalid AssetId)");

        if (!seenIds.insert(id).second)
            return std::unexpected(fmt::format("asset id {} duplicated", id));

        if (!entry.contains("type") || !entry["type"].is_string())
            return std::unexpected("missing or invalid 'type'");

        const string typeStr = entry["type"].get<string>();
        const optional<AssetType> type = ParseAssetType(typeStr);
        if (!type)
            return std::unexpected(fmt::format("unknown type '{}'", typeStr));

        const auto importerIt = m_Importers.find(*type);
        if (importerIt == m_Importers.end())
            return std::unexpected(fmt::format("no importer registered for type '{}'", typeStr));

        const Result<vector<u8>> blob = importerIt->second->Cook(context, entry);
        if (!blob)
            return std::unexpected(blob.error());

        writer.Add(AssetId{.Value = id}, *type, *blob);
        return {};
    }
}
