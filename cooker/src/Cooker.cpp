#include <Veng/Cook/Cooker.h>

#include <filesystem>
#include <fstream>
#include <span>
#include <sstream>

#include <fmt/format.h>
#include <xxhash.h>
#include <zstd.h>

#include <Veng/Project/CompressionFormat.h>
#include <Veng/Project/CompressionRole.h>

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

        // Default zstd compression level. A balanced choice for a one-time build artifact; the
        // inflate cost is unaffected by the level. A build configuration overrides it through its
        // CompressionLevel; the zero-config cook uses this default.
        constexpr int ZstdLevel = ZSTD_CLEVEL_DEFAULT;

        // Adds a blob to the archive, compressing it with zstd at `level` and storing whichever of
        // the raw or compressed bytes is smaller. The content hash covers the stored bytes, so
        // verify re-hashes exactly what is on disk. Routes both cooker Add sites through one
        // path so every blob is considered for compression by construction; an already
        // incompressible blob keeps the raw, zero-copy resolve path.
        void EmitBlob(ArchiveWriter& writer, AssetId id, AssetType type, std::span<const u8> blob,
                      int level)
        {
            const usize bound = ZSTD_compressBound(blob.size());
            vector<u8> compressed(bound);
            const usize produced = ZSTD_compress(compressed.data(), compressed.size(), blob.data(),
                                                 blob.size(), level);

            if (ZSTD_isError(produced) == 0u && produced < blob.size())
            {
                compressed.resize(produced);
                writer.Add(id, type, compressed, Xxh3_128(compressed), ArchiveCodec::Zstd,
                           blob.size());
                return;
            }

            writer.Add(id, type, blob, Xxh3_128(blob));
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
            if (name == "material_instance")
            {
                return AssetType::MaterialInstance;
            }
            if (name == "vertex_layout")
            {
                return AssetType::VertexLayout;
            }
            if (name == "prefab")
            {
                return AssetType::Prefab;
            }
            if (name == "level")
            {
                return AssetType::Level;
            }
            if (name == "skeleton")
            {
                return AssetType::Skeleton;
            }
            if (name == "animation")
            {
                return AssetType::Animation;
            }
            if (name == "environment")
            {
                return AssetType::Environment;
            }

            return std::nullopt;
        }

        // The canonical authoring name of a CompressionFormat. The names match the spellings in
        // Veng::ToString(CompressionFormat); duplicated here rather than calling that libveng symbol,
        // since Cooker.cpp links into the veng-free core (and the core-pack bootstrap) that cannot
        // reference libveng's out-of-line name tables.
        string_view CompressionFormatName(CompressionFormat format)
        {
            switch (format)
            {
            case CompressionFormat::RGBA8Unorm:
                return "RGBA8Unorm";
            case CompressionFormat::RGBA8Srgb:
                return "RGBA8Srgb";
            case CompressionFormat::BC7Unorm:
                return "BC7Unorm";
            case CompressionFormat::BC7Srgb:
                return "BC7Srgb";
            case CompressionFormat::ASTC4x4Unorm:
                return "ASTC4x4Unorm";
            case CompressionFormat::ASTC4x4Srgb:
                return "ASTC4x4Srgb";
            case CompressionFormat::RGBA16Sfloat:
                return "RGBA16Sfloat";
            case CompressionFormat::BC5Unorm:
                return "BC5Unorm";
            case CompressionFormat::BC4Unorm:
                return "BC4Unorm";
            }
            return {};
        }

        // The canonical authoring name of a CompressionRole, mirroring Veng::ToString(CompressionRole)
        // for the same veng-free-link reason as CompressionFormatName.
        string_view CompressionRoleName(CompressionRole role)
        {
            switch (role)
            {
            case CompressionRole::Color:
                return "Color";
            case CompressionRole::Normal:
                return "Normal";
            case CompressionRole::Mask:
                return "Mask";
            case CompressionRole::HDR:
                return "HDR";
            case CompressionRole::UI:
                return "UI";
            }
            return {};
        }

        // Parses a CompressionFormat authoring name to its enumerator over the local name table.
        optional<CompressionFormat> ParseCompressionFormatName(const string& name)
        {
            for (const CompressionFormat format : CompressionFormats)
            {
                if (CompressionFormatName(format) == name)
                {
                    return format;
                }
            }
            return std::nullopt;
        }

        // Writes the format a role resolves to into the fixed RoleToFormat record.
        void SetRoleFormat(RoleToFormat& table, CompressionRole role, CompressionFormat format)
        {
            switch (role)
            {
            case CompressionRole::Color:
                table.Color = format;
                return;
            case CompressionRole::Normal:
                table.Normal = format;
                return;
            case CompressionRole::Mask:
                table.Mask = format;
                return;
            case CompressionRole::HDR:
                table.HDR = format;
                return;
            case CompressionRole::UI:
                table.UI = format;
                return;
            }
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

    Result<BuildConfiguration> ParseBuildConfiguration(const path& configFile)
    {
        const std::ifstream file(configFile, std::ios::binary);
        if (!file)
        {
            return std::unexpected(fmt::format("config '{}': failed to open", configFile.string()));
        }

        std::ostringstream contentStream;
        contentStream << file.rdbuf();
        const json cfg = json::parse(contentStream.str(), nullptr, false);
        if (cfg.is_discarded() || !cfg.is_object())
        {
            return std::unexpected(fmt::format("config '{}': invalid JSON", configFile.string()));
        }

        BuildConfiguration config;
        if (cfg.contains("name") && cfg["name"].is_string())
        {
            config.Name = cfg["name"].get<string>();
        }
        if (cfg.contains("target") && cfg["target"].is_string())
        {
            config.Target = cfg["target"].get<string>();
        }
        if (cfg.contains("outputSuffix") && cfg["outputSuffix"].is_string())
        {
            config.OutputSuffix = cfg["outputSuffix"].get<string>();
        }
        if (cfg.contains("compressionLevel") && cfg["compressionLevel"].is_number_integer())
        {
            config.CompressionLevel = cfg["compressionLevel"].get<i32>();
        }

        // The role → format table. Each role maps to a CompressionFormat by name; a role absent
        // from "formats" keeps its RoleToFormat default. The enums serialize by name, never ordinal.
        if (cfg.contains("formats") && cfg["formats"].is_object())
        {
            const json& formats = cfg["formats"];
            for (const CompressionRole role : CompressionRoles)
            {
                const string roleName{CompressionRoleName(role)};
                if (!formats.contains(roleName))
                {
                    continue;
                }
                if (!formats[roleName].is_string())
                {
                    return std::unexpected(fmt::format("config '{}': formats.{} is not a string",
                                                       configFile.string(), roleName));
                }
                const string formatName = formats[roleName].get<string>();
                const optional<CompressionFormat> format = ParseCompressionFormatName(formatName);
                if (!format)
                {
                    return std::unexpected(
                        fmt::format("config '{}': formats.{}: unknown format '{}'",
                                    configFile.string(), roleName, formatName));
                }
                SetRoleFormat(config.Formats, role, *format);
            }
        }

        return config;
    }

    Result<CookProject> ParseProject(const path& projectFile)
    {
        const std::ifstream file(projectFile, std::ios::binary);
        if (!file)
        {
            return std::unexpected(
                fmt::format("project '{}': failed to open", projectFile.string()));
        }

        std::ostringstream contentStream;
        contentStream << file.rdbuf();
        const json project = json::parse(contentStream.str(), nullptr, false);
        if (project.is_discarded() || !project.is_object())
        {
            return std::unexpected(fmt::format("project '{}': invalid JSON", projectFile.string()));
        }

        CookProject parsed;
        parsed.Directory = projectFile.parent_path();

        // packs and configurations are file paths relative to the project file's directory.
        const auto resolveList = [&](const char* key, vector<path>& out) -> VoidResult
        {
            if (!project.contains(key))
            {
                return {};
            }
            if (!project[key].is_array())
            {
                return std::unexpected(
                    fmt::format("project '{}': '{}' is not an array", projectFile.string(), key));
            }
            for (const json& entry : project[key])
            {
                if (!entry.is_string())
                {
                    return std::unexpected(fmt::format("project '{}': '{}' entry is not a string",
                                                       projectFile.string(), key));
                }
                out.push_back(parsed.Directory / entry.get<string>());
            }
            return {};
        };

        if (const VoidResult packs = resolveList("packs", parsed.Packs); !packs)
        {
            return std::unexpected(packs.error());
        }
        if (const VoidResult configs = resolveList("configurations", parsed.ConfigFiles); !configs)
        {
            return std::unexpected(configs.error());
        }

        if (project.contains("activeConfiguration") && project["activeConfiguration"].is_string())
        {
            parsed.ActiveConfiguration = project["activeConfiguration"].get<string>();
        }

        if (project.contains("startupLevel"))
        {
            if (!project["startupLevel"].is_number_unsigned())
            {
                return std::unexpected(fmt::format(
                    "project '{}': startupLevel is not an unsigned integer", projectFile.string()));
            }
            parsed.StartupLevel = AssetId{.Value = project["startupLevel"].get<u64>()};
        }

        return parsed;
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
                                const SystemRegistry* systems, vector<path>* outDependencies,
                                const BuildConfiguration* config, const path& configFile,
                                const path& shaderIncludeDir) const
    {
        const Result<json> packResult = ReadAndValidatePack(packJson);
        if (!packResult)
        {
            return std::unexpected(packResult.error());
        }

        const json& pack = *packResult;

        // Prefab and level entries require --module for their reflected descriptors.
        // Check before full entry parsing so the error names the cause.
        if (types == nullptr)
        {
            const json& assets = pack["assets"];
            for (usize index = 0; index < assets.size(); ++index)
            {
                const json& entry = assets[index];
                if (entry.is_object() && entry.contains("type") && entry["type"].is_string())
                {
                    const string typeStr = entry["type"].get<string>();
                    if (typeStr == "prefab" || typeStr == "level")
                    {
                        return std::unexpected(
                            fmt::format("pack '{}': asset[{}]: {} cooking requires --module",
                                        packJson.string(), index, typeStr));
                    }
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

        // The configuration file is one central depfile input, recorded centrally like the pack
        // JSON: a configuration edit re-cooks the whole pack — coarse by design, since the texture
        // encode is the expensive part and the rest of a re-cook is fast.
        if (config != nullptr && !configFile.empty())
        {
            record(configFile);
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
            .Systems = systems,
            .Config = config,
            .ShaderIncludeDir = shaderIncludeDir,
            .RecordDependency = record,
        };

        // The configuration drives the archive compression level; the zero-config cook uses the
        // default. This is the one place the level field is consumed.
        const int level = config != nullptr ? config->CompressionLevel : ZstdLevel;

        ArchiveWriter writer;
        std::set<u64> seenIds;

        const json& assets = pack["assets"];
        for (usize index = 0; index < assets.size(); ++index)
        {
            const VoidResult entryResult =
                CookEntry(context, assets[index], seenIds, writer, level);
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
                                          const TypeRegistry* types, const SystemRegistry* systems,
                                          const BuildConfiguration* config,
                                          const path& shaderIncludeDir) const
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

        // CookSource writes no files, so RecordDependency is a no-op. The configuration, when
        // supplied, drives role→format resolution exactly as a file-based cook does; a null
        // configuration uses the zero-config defaults.
        const CookContext context{
            .PackDir = sourcePath.parent_path(),
            .Resolve = resolve,
            .Types = types,
            .Systems = systems,
            .Config = config,
            .ShaderIncludeDir = shaderIncludeDir,
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
        EmitBlob(writer, id, type, *blob, ZstdLevel);

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
                                 std::set<u64>& seenIds, ArchiveWriter& writer, int level) const
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

        EmitBlob(writer, AssetId{.Value = id}, *type, *blob, level);
        return {};
    }
}
