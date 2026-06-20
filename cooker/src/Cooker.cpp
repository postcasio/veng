#include <Veng/Cook/Cooker.h>

#include <filesystem>
#include <fstream>
#include <span>
#include <sstream>

#include <fmt/format.h>
#include <xxhash.h>

namespace Veng::Cook
{
    namespace
    {
        // Normalizes a recorded dependency to a stable absolute form so the same
        // file reached two ways (a relative source vs. a resolved reference)
        // de-duplicates to one depfile entry. weakly_canonical tolerates a path
        // that does not exist; on failure it falls back to lexical normalization.
        path NormalizeDependency(const path& p)
        {
            std::error_code ec;
            const path canonical = std::filesystem::weakly_canonical(p, ec);
            if (ec || canonical.empty())
            {
                return p.lexically_normal();
            }
            return canonical;
        }

        // xxh3-128 of a byte range, packed into the format's ContentHash.
        ContentHash Xxh3_128(std::span<const u8> bytes)
        {
            const XXH128_hash_t h = XXH3_128bits(bytes.data(), bytes.size());
            return ContentHash{.Lo = h.low64, .Hi = h.high64};
        }

        optional<AssetType> ParseAssetType(const string& name)
        {
            if (name == "raw")
            {
                return AssetType::Raw;
            }
            if (name == "texture")
            {
                return AssetType::Texture;
            }
            if (name == "mesh")
            {
                return AssetType::Mesh;
            }
            if (name == "shader")
            {
                return AssetType::Shader;
            }
            if (name == "material")
            {
                return AssetType::Material;
            }
            if (name == "vertex_layout")
            {
                return AssetType::VertexLayout;
            }
            if (name == "prefab")
            {
                return AssetType::Prefab;
            }

            return std::nullopt;
        }

        // Parses and validates the common pack JSON preamble. On error returns a located message.
        Result<json> ReadAndValidatePack(const path& packJson)
        {
            const std::ifstream file(packJson, std::ios::binary);
            if (!file)
            {
                return std::unexpected(fmt::format("pack '{}': failed to open", packJson.string()));
            }

            std::ostringstream contentStream;
            contentStream << file.rdbuf();
            const string content = contentStream.str();

            const json pack = json::parse(content, nullptr, false);
            if (pack.is_discarded())
            {
                return std::unexpected(fmt::format("pack '{}': invalid JSON", packJson.string()));
            }

            if (!pack.is_object() || !pack.contains("version") ||
                !pack["version"].is_number_unsigned())
            {
                return std::unexpected(
                    fmt::format("pack '{}': missing or invalid 'version'", packJson.string()));
            }

            const u64 version = pack["version"].get<u64>();
            if (version != 1)
            {
                return std::unexpected(fmt::format("pack '{}': unsupported version {} (expected 1)",
                                                   packJson.string(), version));
            }

            if (!pack.contains("assets") || !pack["assets"].is_array())
            {
                return std::unexpected(
                    fmt::format("pack '{}': missing 'assets' array", packJson.string()));
            }

            return pack;
        }
    }

    Result<AssetPack> ParseAssetPack(const path& packJson)
    {
        const Result<json> packResult = ReadAndValidatePack(packJson);
        if (!packResult)
        {
            return std::unexpected(packResult.error());
        }

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
                return std::unexpected(fmt::format("pack '{}': asset[{}]: entry is not an object",
                                                   packJson.string(), index));
            }

            if (!entry.contains("id") || !entry["id"].is_number_unsigned())
            {
                return std::unexpected(fmt::format("pack '{}': asset[{}]: missing or invalid 'id'",
                                                   packJson.string(), index));
            }

            const u64 id = entry["id"].get<u64>();
            if (id == 0)
            {
                return std::unexpected(fmt::format("pack '{}': asset[{}]: asset id 0 is reserved",
                                                   packJson.string(), index));
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
                return std::unexpected(fmt::format("pack '{}': asset[{}]: unknown type '{}'",
                                                   packJson.string(), index, typeStr));
            }

            string source;
            if (entry.contains("source") && entry["source"].is_string())
            {
                source = entry["source"].get<string>();
            }

            result.Entries.push_back(AssetPackEntry{
                .Id = AssetId{.Value = id},
                .Type = *type,
                .Source = std::move(source),
            });
        }

        return result;
    }

    VoidResult WriteDepfile(const path& depfilePath, const path& target,
                            std::span<const path> dependencies)
    {
        std::ofstream out(depfilePath, std::ios::binary | std::ios::trunc);
        if (!out)
        {
            return std::unexpected(
                fmt::format("depfile '{}': failed to open for writing", depfilePath.string()));
        }

        // GCC/Make escaping: a space or '#' in a filename is backslash-escaped,
        // a '$' is doubled. Path separators (incl. Windows '\\') pass through.
        auto escape = [](const path& p) -> string
        {
            const string raw = p.string();
            string escaped;
            escaped.reserve(raw.size());
            for (const char c : raw)
            {
                if (c == ' ' || c == '#')
                {
                    escaped.push_back('\\');
                }
                else if (c == '$')
                {
                    escaped.push_back('$');
                }
                escaped.push_back(c);
            }
            return escaped;
        };

        out << escape(target) << ':';
        for (const path& dep : dependencies)
        {
            out << " \\\n  " << escape(dep);
        }
        out << '\n';

        if (!out)
        {
            return std::unexpected(fmt::format("depfile '{}': write failed", depfilePath.string()));
        }

        return {};
    }

    void Cooker::Register(Unique<AssetImporter> importer)
    {
        const AssetType type = importer->Type();
        m_Importers[type] = std::move(importer);
    }

    VoidResult Cooker::CookPack(const path& packJson, const path& outArchive,
                                std::span<const path> referencePacks, const TypeRegistry* types,
                                vector<path>* outDependencies) const
    {
        const Result<json> packResult = ReadAndValidatePack(packJson);
        if (!packResult)
        {
            return std::unexpected(packResult.error());
        }

        const json& pack = *packResult;

        // Prefab entries require --module for their reflected component descriptors.
        // Check before full entry parsing so the error names the cause.
        if (types == nullptr)
        {
            const json& assets = pack["assets"];
            for (usize index = 0; index < assets.size(); ++index)
            {
                const json& entry = assets[index];
                if (entry.is_object() && entry.contains("type") && entry["type"].is_string() &&
                    entry["type"].get<string>() == "prefab")
                {
                    return std::unexpected(
                        fmt::format("pack '{}': asset[{}]: prefab cooking requires --module",
                                    packJson.string(), index));
                }
            }
        }

        const Result<AssetPack> mainPackResult = ParseAssetPack(packJson);
        if (!mainPackResult)
        {
            return std::unexpected(mainPackResult.error());
        }

        vector<AssetPack> refPacks;
        refPacks.reserve(referencePacks.size());
        for (const path& refPath : referencePacks)
        {
            Result<AssetPack> refPackResult = ParseAssetPack(refPath);
            if (!refPackResult)
            {
                return std::unexpected(fmt::format("pack '{}': reference pack error: {}",
                                                   packJson.string(), refPackResult.error()));
            }
            refPacks.push_back(std::move(*refPackResult));
        }

        // std::set keeps dependencies sorted and de-duplicated.
        std::set<path> dependencies;
        auto record = [&dependencies](const path& p)
        { dependencies.insert(NormalizeDependency(p)); };

        record(packJson);
        for (const path& refPath : referencePacks)
        {
            record(refPath);
        }

        // Resolve searches the main pack first, then reference packs in order.
        const AssetPack& mainPack = *mainPackResult;
        auto resolve = [&mainPack, &refPacks, &record](AssetId id) -> optional<ResolvedSource>
        {
            if (const AssetPackEntry* e = mainPack.FindById(id))
            {
                if (e->Source.empty())
                {
                    return std::nullopt;
                }
                const path absolute = mainPack.Dir / e->Source;
                record(absolute);
                return ResolvedSource{.AbsolutePath = absolute, .Type = e->Type};
            }
            for (const AssetPack& ref : refPacks)
            {
                if (const AssetPackEntry* e = ref.FindById(id))
                {
                    if (e->Source.empty())
                    {
                        return std::nullopt;
                    }
                    const path absolute = ref.Dir / e->Source;
                    record(absolute);
                    return ResolvedSource{.AbsolutePath = absolute, .Type = e->Type};
                }
            }
            return std::nullopt;
        };

        const CookContext context{
            .PackDir = packJson.parent_path(),
            .Resolve = resolve,
            .Types = types,
            .RecordDependency = record,
        };

        ArchiveWriter writer;
        std::set<u64> seenIds;

        const json& assets = pack["assets"];
        for (usize index = 0; index < assets.size(); ++index)
        {
            const VoidResult entryResult = CookEntry(context, assets[index], seenIds, writer);
            if (!entryResult)
            {
                return std::unexpected(fmt::format("pack '{}': asset[{}]: {}", packJson.string(),
                                                   index, entryResult.error()));
            }
        }

        // Build once to lay out the TOC bytes, hash them via the reader, then
        // rebuild with the digest set. The header's ArchiveDigest field is
        // excluded from the hashed range so the second build is stable.
        const vector<u8> staged = writer.Build();
        const Result<ArchiveReader> reader = ArchiveReader::FromBytes(staged);
        if (!reader)
        {
            return std::unexpected(fmt::format("pack '{}': {}", packJson.string(), reader.error()));
        }

        writer.SetArchiveDigest(Xxh3_128(reader->TocBytes()));

        if (outDependencies)
        {
            outDependencies->assign(dependencies.begin(), dependencies.end());
        }

        return writer.Write(outArchive);
    }

    Result<vector<u8>> Cooker::CookSource(const path& sourcePath, AssetId id, AssetType type,
                                          std::span<const path> referencePacks,
                                          const TypeRegistry* types) const
    {
        const auto importerIt = m_Importers.find(type);
        if (importerIt == m_Importers.end())
        {
            return std::unexpected(fmt::format(
                "cook '{}': no importer registered for the requested type", sourcePath.string()));
        }

        vector<AssetPack> refPacks;
        refPacks.reserve(referencePacks.size());
        for (const path& refPath : referencePacks)
        {
            Result<AssetPack> refPackResult = ParseAssetPack(refPath);
            if (!refPackResult)
            {
                return std::unexpected(fmt::format("cook '{}': reference pack error: {}",
                                                   sourcePath.string(), refPackResult.error()));
            }
            refPacks.push_back(std::move(*refPackResult));
        }

        auto resolve = [&refPacks](AssetId resolveId) -> optional<ResolvedSource>
        {
            for (const AssetPack& ref : refPacks)
            {
                if (const AssetPackEntry* e = ref.FindById(resolveId))
                {
                    if (e->Source.empty())
                    {
                        return std::nullopt;
                    }
                    return ResolvedSource{.AbsolutePath = ref.Dir / e->Source, .Type = e->Type};
                }
            }
            return std::nullopt;
        };

        // CookSource writes no files, so RecordDependency is a no-op.
        const CookContext context{
            .PackDir = sourcePath.parent_path(),
            .Resolve = resolve,
            .Types = types,
            .RecordDependency = [](const path&) {},
        };

        json entry;
        entry["source"] = sourcePath.filename().string();

        const Result<vector<u8>> blob = importerIt->second->Cook(context, entry);
        if (!blob)
        {
            return std::unexpected(fmt::format("cook '{}': {}", sourcePath.string(), blob.error()));
        }

        ArchiveWriter writer;
        writer.Add(id, type, *blob, Xxh3_128(*blob));

        const vector<u8> staged = writer.Build();
        const Result<ArchiveReader> reader = ArchiveReader::FromBytes(staged);
        if (!reader)
        {
            return std::unexpected(
                fmt::format("cook '{}': {}", sourcePath.string(), reader.error()));
        }

        writer.SetArchiveDigest(Xxh3_128(reader->TocBytes()));
        return writer.Build();
    }

    VoidResult Cooker::CookEntry(const CookContext& context, const json& entry,
                                 std::set<u64>& seenIds, ArchiveWriter& writer) const
    {
        if (!entry.is_object())
        {
            return std::unexpected("entry is not an object");
        }

        if (!entry.contains("id") || !entry["id"].is_number_unsigned())
        {
            return std::unexpected("missing or invalid 'id' (expected a non-zero u64)");
        }

        const u64 id = entry["id"].get<u64>();
        if (id == 0)
        {
            return std::unexpected("asset id 0 is reserved (invalid AssetId)");
        }

        if (!seenIds.insert(id).second)
        {
            return std::unexpected(fmt::format("asset id {} duplicated", id));
        }

        if (!entry.contains("type") || !entry["type"].is_string())
        {
            return std::unexpected("missing or invalid 'type'");
        }

        const string typeStr = entry["type"].get<string>();
        const optional<AssetType> type = ParseAssetType(typeStr);
        if (!type)
        {
            return std::unexpected(fmt::format("unknown type '{}'", typeStr));
        }

        const auto importerIt = m_Importers.find(*type);
        if (importerIt == m_Importers.end())
        {
            return std::unexpected(fmt::format("no importer registered for type '{}'", typeStr));
        }

        // Record the per-asset JSON source; importers record their binary payloads.
        if (entry.contains("source") && entry["source"].is_string())
        {
            context.RecordDependency(context.PackDir / entry["source"].get<string>());
        }

        const Result<vector<u8>> blob = importerIt->second->Cook(context, entry);
        if (!blob)
        {
            return std::unexpected(blob.error());
        }

        writer.Add(AssetId{.Value = id}, *type, *blob, Xxh3_128(*blob));
        return {};
    }
}
