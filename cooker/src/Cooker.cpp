#include <Veng/Cook/Cooker.h>

#include <fstream>
#include <span>
#include <sstream>

#include <fmt/format.h>
#include <xxhash.h>

namespace Veng::Cook
{
    namespace
    {
        // xxh3-128 of a byte range, packed into the format's ContentHash.
        ContentHash Xxh3_128(std::span<const u8> bytes)
        {
            const XXH128_hash_t h = XXH3_128bits(bytes.data(), bytes.size());
            return ContentHash{.Lo = h.low64, .Hi = h.high64};
        }

        optional<AssetType> ParseAssetType(const string& name)
        {
            if (name == "raw")           return AssetType::Raw;
            if (name == "texture")       return AssetType::Texture;
            if (name == "mesh")          return AssetType::Mesh;
            if (name == "shader")        return AssetType::Shader;
            if (name == "material")      return AssetType::Material;
            if (name == "vertex_layout") return AssetType::VertexLayout;
            if (name == "prefab")        return AssetType::Prefab;

            return std::nullopt;
        }

        // Parse the common pack JSON preamble and return the json object.
        // On error returns std::unexpected with a located message.
        Result<json> ReadAndValidatePack(const path& packJson)
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

            return pack;
        }
    }

    Result<AssetPack> ParseAssetPack(const path& packJson)
    {
        const Result<json> packResult = ReadAndValidatePack(packJson);
        if (!packResult)
            return std::unexpected(packResult.error());

        const json& pack = *packResult;
        const json& assets = pack["assets"];

        AssetPack result;
        result.Dir = packJson.parent_path();
        result.Entries.reserve(assets.size());

        for (usize index = 0; index < assets.size(); ++index)
        {
            const json& entry = assets[index];
            if (!entry.is_object())
            {
                return std::unexpected(fmt::format(
                    "pack '{}': asset[{}]: entry is not an object", packJson.string(), index));
            }

            if (!entry.contains("id") || !entry["id"].is_number_unsigned())
            {
                return std::unexpected(fmt::format(
                    "pack '{}': asset[{}]: missing or invalid 'id'", packJson.string(), index));
            }

            const u64 id = entry["id"].get<u64>();
            if (id == 0)
            {
                return std::unexpected(fmt::format(
                    "pack '{}': asset[{}]: asset id 0 is reserved", packJson.string(), index));
            }

            if (!entry.contains("type") || !entry["type"].is_string())
            {
                return std::unexpected(fmt::format(
                    "pack '{}': asset[{}]: missing or invalid 'type'", packJson.string(), index));
            }

            const string typeStr = entry["type"].get<string>();
            const optional<AssetType> type = ParseAssetType(typeStr);
            if (!type)
            {
                return std::unexpected(fmt::format(
                    "pack '{}': asset[{}]: unknown type '{}'", packJson.string(), index, typeStr));
            }

            string source;
            if (entry.contains("source") && entry["source"].is_string())
                source = entry["source"].get<string>();

            result.Entries.push_back(AssetPackEntry{
                .Id = AssetId{.Value = id},
                .Type = *type,
                .Source = std::move(source),
            });
        }

        return result;
    }

    void Cooker::Register(Unique<AssetImporter> importer)
    {
        const AssetType type = importer->Type();
        m_Importers[type] = std::move(importer);
    }

    VoidResult Cooker::CookPack(const path& packJson, const path& outArchive,
        std::span<const path> referencePacks, const TypeRegistry* types) const
    {
        const Result<json> packResult = ReadAndValidatePack(packJson);
        if (!packResult)
            return std::unexpected(packResult.error());

        const json& pack = *packResult;

        // A prefab entry cooks against the loaded module's reflected component
        // descriptors, so it requires --module. Caught here before resolution
        // parsing (which classifies entry types) so the message names the cause.
        if (types == nullptr)
        {
            const json& assets = pack["assets"];
            for (usize index = 0; index < assets.size(); ++index)
            {
                const json& entry = assets[index];
                if (entry.is_object() && entry.contains("type") && entry["type"].is_string()
                    && entry["type"].get<string>() == "prefab")
                {
                    return std::unexpected(fmt::format(
                        "pack '{}': asset[{}]: prefab cooking requires --module",
                        packJson.string(), index));
                }
            }
        }

        // Build the main AssetPack for resolution.
        const Result<AssetPack> mainPackResult = ParseAssetPack(packJson);
        if (!mainPackResult)
            return std::unexpected(mainPackResult.error());

        // Parse all reference packs.
        vector<AssetPack> refPacks;
        refPacks.reserve(referencePacks.size());
        for (const path& refPath : referencePacks)
        {
            Result<AssetPack> refPackResult = ParseAssetPack(refPath);
            if (!refPackResult)
            {
                return std::unexpected(fmt::format(
                    "pack '{}': reference pack error: {}", packJson.string(), refPackResult.error()));
            }
            refPacks.push_back(std::move(*refPackResult));
        }

        // Build the Resolve closure: search the main pack first, then references.
        // Returns the absolute path (pack.Dir / entry.Source) and type.
        const AssetPack& mainPack = *mainPackResult;
        auto resolve = [&mainPack, &refPacks](AssetId id) -> optional<ResolvedSource>
        {
            // Search main pack first.
            if (const AssetPackEntry* e = mainPack.FindById(id))
            {
                if (e->Source.empty())
                    return std::nullopt;
                return ResolvedSource{.AbsolutePath = mainPack.Dir / e->Source, .Type = e->Type};
            }
            // Then reference packs in order.
            for (const AssetPack& ref : refPacks)
            {
                if (const AssetPackEntry* e = ref.FindById(id))
                {
                    if (e->Source.empty())
                        return std::nullopt;
                    return ResolvedSource{.AbsolutePath = ref.Dir / e->Source, .Type = e->Type};
                }
            }
            return std::nullopt;
        };

        const CookContext context{
            .PackDir = packJson.parent_path(),
            .Resolve = resolve,
            .Types = types,
        };

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

        // The archive digest covers the serialized TOC byte region — the
        // contiguous run of on-disk TOC entries (id/type/offset/size + each
        // per-blob hash) between the header and the blob region. Hashing the
        // bytes in their on-disk order (Build() emits them id-sorted) means
        // verify re-hashes the same bytes with no separate sort step, and the
        // header's ArchiveDigest field is not part of the hashed range. Build
        // once to lay out those bytes, read them back through the reader, hash,
        // then rebuild with the digest set.
        const vector<u8> staged = writer.Build();
        const Result<ArchiveReader> reader = ArchiveReader::FromBytes(staged);
        if (!reader)
            return std::unexpected(fmt::format("pack '{}': {}", packJson.string(), reader.error()));

        writer.SetArchiveDigest(Xxh3_128(reader->TocBytes()));

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

        writer.Add(AssetId{.Value = id}, *type, *blob, Xxh3_128(*blob));
        return {};
    }
}
