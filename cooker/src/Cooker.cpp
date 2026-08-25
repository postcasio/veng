#include <Veng/Cook/Cooker.h>

#include <algorithm>
#include <atomic>
#include <cstdlib>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <span>
#include <sstream>
#include <thread>

#include <fmt/format.h>
#include <xxhash.h>
#include <zstd.h>

#include <Veng/Asset/AtomicFile.h>
#include <Veng/Asset/HexId.h>
#include <Veng/Cook/CookCache.h>
#include <Veng/Cook/CookTiming.h>
#include <Veng/Cook/JsonFile.h>
#include <Veng/Project/CompressionFormat.h>
#include <Veng/Project/CompressionRole.h>

#include "Importers/MaterialInstanceImporter.h"

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

        // The cooker core links no libveng, so VE_ASSERT is out of reach here. An importer
        // registration conflict is unrecoverable authoring misuse, so it aborts the same way the
        // asset-type registry's own collision does.
        [[noreturn]] void FatalRegistration(const string& message)
        {
            fmt::print(stderr, "Cooker: {}\n", message);
            std::abort();
        }

        // A manifest naming a type nothing registered is what a forgotten --module produces, so the
        // error names the flag rather than leaving the reader to suspect the manifest, and lists
        // the names that did resolve.
        string UnknownTypeReason(const string& typeName, const AssetTypeRegistry& types)
        {
            vector<string> known;
            known.reserve(types.All().size());
            for (const auto& [id, info] : types.All())
            {
                known.push_back(info.Name);
            }
            std::ranges::sort(known);

            string list;
            for (const string& name : known)
            {
                if (!list.empty())
                {
                    list += ", ";
                }
                list += name;
            }

            return fmt::format(
                "unknown type '{}': no asset type of that name is registered with this cook. A "
                "game-defined type's name comes from its runtime module, so pass --module (or "
                "--cook-module, which implies it) when the type is the game's. Registered types: "
                "{}",
                typeName, list);
        }

        // Runs `task` once per entry of `indices`, across at most `workers` threads: an atomic cursor
        // hands each thread the next index, so a long asset never leaves the others idle behind a
        // fixed partition. One worker (or one index) runs the whole set on the calling thread and
        // spawns nothing, which keeps a single-job cook exactly as sequential as it reads.
        void RunIndexed(std::span<const usize> indices, u32 workers,
                        const function<void(usize)>& task)
        {
            if (workers <= 1 || indices.size() <= 1)
            {
                for (const usize index : indices)
                {
                    task(index);
                }
                return;
            }

            std::atomic<usize> next{0};
            const auto drain = [&]
            {
                for (usize i = next.fetch_add(1); i < indices.size(); i = next.fetch_add(1))
                {
                    task(indices[i]);
                }
            };

            const usize spawned = std::min<usize>(workers, indices.size()) - 1;
            vector<std::thread> pool;
            pool.reserve(spawned);
            for (usize i = 0; i < spawned; ++i)
            {
                pool.emplace_back(drain);
            }
            drain();
            for (std::thread& worker : pool)
            {
                worker.join();
            }
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

        // Turns a raw cooked blob into its stored form: compresses it with zstd at `level` and keeps
        // whichever of the raw or compressed bytes is smaller. The content hash covers the stored
        // bytes, so verify re-hashes exactly what is on disk; an already incompressible blob keeps
        // the raw, zero-copy resolve path. This is the one place a blob is considered for
        // compression, so both the fresh-cook and cache-store paths agree on the stored bytes.
        CachedBlob MakeStoredBlob(AssetId id, AssetTypeId type, std::span<const u8> blob, int level)
        {
            const usize bound = ZSTD_compressBound(blob.size());
            vector<u8> compressed(bound);
            const usize produced = ZSTD_compress(compressed.data(), compressed.size(), blob.data(),
                                                 blob.size(), level);

            if (ZSTD_isError(produced) == 0u && produced < blob.size())
            {
                compressed.resize(produced);
                return CachedBlob{.Id = id,
                                  .Type = type,
                                  .Codec = ArchiveCodec::Zstd,
                                  .UncompressedSize = blob.size(),
                                  .Hash = Xxh3_128(compressed),
                                  .Bytes = std::move(compressed)};
            }

            return CachedBlob{.Id = id,
                              .Type = type,
                              .Codec = ArchiveCodec::Stored,
                              .UncompressedSize = blob.size(),
                              .Hash = Xxh3_128(blob),
                              .Bytes = vector<u8>(blob.begin(), blob.end())};
        }

        // Adds an already-stored blob to the archive verbatim, passing the codec and inflated length
        // through. Serves both a freshly compressed blob and one replayed from the cache.
        void AddStored(ArchiveWriter& writer, const CachedBlob& blob)
        {
            writer.Add(blob.Id, blob.Type, blob.Bytes, blob.Hash, blob.Codec,
                       blob.UncompressedSize);
        }

        // Parses and validates the common pack JSON preamble. On error returns a located message.
        Result<json> ReadAndValidatePack(const path& packJson)
        {
            const Result<json> packResult = ReadJsonFile(packJson, "pack");
            if (!packResult)
            {
                return std::unexpected(packResult.error());
            }
            const json& pack = *packResult;

            if (!pack.contains("version") || !pack["version"].is_number_unsigned())
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

    Result<AssetPack> ParseAssetPack(const path& packJson, const AssetTypeRegistry& types)
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

            if (!entry.contains("id") || !entry["id"].is_string())
            {
                return std::unexpected(fmt::format(
                    "pack '{}': asset[{}]: missing or invalid 'id' (expected hex id string)",
                    packJson.string(), index));
            }

            const optional<AssetId> parsedId = ParseAssetId(entry["id"].get<string>());
            if (!parsedId)
            {
                return std::unexpected(fmt::format("pack '{}': asset[{}]: malformed hex id '{}'",
                                                   packJson.string(), index,
                                                   entry["id"].get<string>()));
            }

            const u64 id = parsedId->Value;
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
            const optional<AssetTypeId> type = types.FindByName(typeStr);
            if (!type)
            {
                return std::unexpected(fmt::format("pack '{}': asset[{}]: {}", packJson.string(),
                                                   index, UnknownTypeReason(typeStr, types)));
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

    Result<AssetId> GenerateAssetId(std::span<const path> referencePackPaths,
                                    const AssetTypeRegistry& types)
    {
        vector<AssetPack> packs;
        packs.reserve(referencePackPaths.size());
        for (const path& refPath : referencePackPaths)
        {
            Result<AssetPack> packResult = ParseAssetPack(refPath, types);
            if (!packResult)
            {
                return std::unexpected(packResult.error());
            }
            packs.push_back(std::move(*packResult));
        }

        vector<const AssetPack*> packPtrs;
        packPtrs.reserve(packs.size());
        for (const AssetPack& pack : packs)
        {
            packPtrs.push_back(&pack);
        }

        return GenerateAssetId(std::span<const AssetPack* const>(packPtrs));
    }

    Result<BuildConfiguration> ParseBuildConfiguration(const path& configFile)
    {
        const Result<json> cfgResult = ReadJsonFile(configFile, "config");
        if (!cfgResult)
        {
            return std::unexpected(cfgResult.error());
        }
        const json& cfg = *cfgResult;

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
                const string roleName{ToString(role)};
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
                const optional<CompressionFormat> format = ParseCompressionFormat(formatName);
                if (!format)
                {
                    return std::unexpected(
                        fmt::format("config '{}': formats.{}: unknown format '{}'",
                                    configFile.string(), roleName, formatName));
                }
                config.Formats.SetFormat(role, *format);
            }
        }

        return config;
    }

    Result<CookProject> ParseProject(const path& projectFile)
    {
        const Result<json> projectResult = ReadJsonFile(projectFile, "project");
        if (!projectResult)
        {
            return std::unexpected(projectResult.error());
        }
        const json& project = *projectResult;

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
            if (!project["startupLevel"].is_string())
            {
                return std::unexpected(fmt::format(
                    "project '{}': startupLevel is not a hex id string", projectFile.string()));
            }
            const optional<AssetId> startup = ParseAssetId(project["startupLevel"].get<string>());
            if (!startup)
            {
                return std::unexpected(
                    fmt::format("project '{}': startupLevel is a malformed hex id '{}'",
                                projectFile.string(), project["startupLevel"].get<string>()));
            }
            parsed.StartupLevel = *startup;
        }

        return parsed;
    }

    VoidResult WriteDepfile(const path& depfilePath, const path& target,
                            std::span<const path> dependencies)
    {
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

        string out = escape(target);
        out += ':';
        for (const path& dep : dependencies)
        {
            out += " \\\n  ";
            out += escape(dep);
        }
        out += '\n';

        // Atomic so a killed cook never leaves a truncated depfile behind — a torn
        // dependency list would silently drop re-cook triggers.
        return WriteFileAtomic(depfilePath,
                               std::span(reinterpret_cast<const u8*>(out.data()), out.size()));
    }

    Cooker::Cooker()
    {
        RegisterBuiltinAssetTypes(m_AssetTypes);
    }

    void Cooker::Register(Unique<AssetImporter> importer)
    {
        const ImporterModuleDependence dependence = importer != nullptr
                                                        ? importer->ModuleDependence()
                                                        : ImporterModuleDependence::Independent;
        RegisterImporter(std::move(importer), dependence);
    }

    void Cooker::RegisterFromModule(Unique<AssetImporter> importer)
    {
        RegisterImporter(std::move(importer), ImporterModuleDependence::DependsOnModule);
    }

    void Cooker::RegisterImporter(Unique<AssetImporter> importer,
                                  ImporterModuleDependence dependence)
    {
        if (importer == nullptr)
        {
            FatalRegistration("importer is null");
        }

        const AssetTypeId type = importer->Type();
        if (m_Importers.contains(type))
        {
            FatalRegistration(
                fmt::format("asset type '{}' already has an importer. Override semantics for "
                            "builtin types stay engine-owned, and a silent replacement would cook "
                            "every asset of that type through the newcomer",
                            m_AssetTypes.GetName(type)));
        }

        m_Importers[type] =
            RegisteredImporter{.Importer = std::move(importer), .Dependence = dependence};
    }

    ImporterModuleDependence Cooker::EntryModuleDependence(const json& entry) const
    {
        if (!entry.is_object() || !entry.contains("type") || !entry["type"].is_string())
        {
            return ImporterModuleDependence::DependsOnModule;
        }
        const optional<AssetTypeId> type = m_AssetTypes.FindByName(entry["type"].get<string>());
        if (!type)
        {
            return ImporterModuleDependence::DependsOnModule;
        }
        const auto importerIt = m_Importers.find(*type);
        if (importerIt == m_Importers.end())
        {
            return ImporterModuleDependence::DependsOnModule;
        }
        return importerIt->second.Dependence;
    }

    VoidResult Cooker::CookPack(const path& packJson, const path& outArchive,
                                std::span<const path> referencePacks, const TypeRegistry* types,
                                const SystemRegistry* systems, vector<path>* outDependencies,
                                const BuildConfiguration* config, const path& configFile,
                                const path& shaderIncludeDir, const CookCache* cache,
                                CookTiming* timing, u32 jobs) const
    {
        const CookStopwatch manifestWatch;

        const Result<json> packResult = ReadAndValidatePack(packJson);
        if (!packResult)
        {
            return std::unexpected(packResult.error());
        }

        const json& pack = *packResult;

        const Result<AssetPack> mainPackResult = ParseAssetPack(packJson, m_AssetTypes);
        if (!mainPackResult)
        {
            return std::unexpected(mainPackResult.error());
        }

        vector<AssetPack> refPacks;
        refPacks.reserve(referencePacks.size());
        for (const path& refPath : referencePacks)
        {
            Result<AssetPack> refPackResult = ParseAssetPack(refPath, m_AssetTypes);
            if (!refPackResult)
            {
                return std::unexpected(fmt::format("pack '{}': reference pack error: {}",
                                                   packJson.string(), refPackResult.error()));
            }
            refPacks.push_back(std::move(*refPackResult));
        }

        if (timing != nullptr)
        {
            timing->ManifestParseSeconds += manifestWatch.Elapsed();
        }

        // std::set keeps dependencies sorted and de-duplicated. Only the central inputs — the pack
        // JSON, the reference packs, the configuration — are recorded here; an entry's own inputs
        // are collected into that entry's slot, because entries cook on worker threads and a shared
        // container written from several at once would race. The slots merge in at the end.
        std::set<path> dependencies;
        const auto record = [&dependencies](const path& p)
        { dependencies.insert(NormalizeDependency(p)); };

        record(packJson);
        for (const path& refPath : referencePacks)
        {
            record(refPath);
        }

        // The configuration file is one central depfile input, recorded centrally like the pack
        // JSON: a configuration edit re-cooks the whole pack — coarse by design, since the texture
        // encode is the expensive part and the rest of a re-cook is fast. The configuration's effect
        // on a per-asset cache entry is captured by its fingerprint in the cache key, not here.
        if (config != nullptr && !configFile.empty())
        {
            record(configFile);
        }

        // resolveById is the pure id → source lookup (main pack first, then references in order),
        // with no recording — the cache validation path re-resolves through it to confirm an id
        // still maps to the same source, and each entry's own resolver wraps it to record what that
        // one entry read. It reads only the parsed packs, so any number of workers may call it.
        const AssetPack& mainPack = *mainPackResult;
        const auto resolveById = [&mainPack, &refPacks](AssetId id) -> optional<ResolvedSource>
        {
            if (const AssetPackEntry* e = mainPack.FindById(id))
            {
                if (e->Source.empty())
                {
                    return std::nullopt;
                }
                return ResolvedSource{.AbsolutePath = mainPack.Dir / e->Source, .Type = e->Type};
            }
            for (const AssetPack& ref : refPacks)
            {
                if (const AssetPackEntry* e = ref.FindById(id))
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
        const path packDir = packJson.parent_path();

        // The configuration drives the archive compression level; the zero-config cook uses the
        // default. This is the one place the level field is consumed.
        const int level = config != nullptr ? config->CompressionLevel : ZstdLevel;

        // Inputs shared by every entry's cache key: the config fingerprint (codec table + level),
        // and the pack + shader-include dirs, computed once. The pack and shader-include dirs are
        // normalized to a canonical absolute form so two spellings of the same directory (a relative
        // vs. absolute --shader-include) key identically rather than each seeding its own entries.
        const string configFingerprint =
            config != nullptr ? FingerprintBuildConfiguration(*config) : string{};
        const path keyPackDir = NormalizeDependency(packDir);
        const path keyShaderIncludeDir =
            shaderIncludeDir.empty() ? path{} : NormalizeDependency(shaderIncludeDir);

        // Content hashes of dependency files, memoized for this cook. Many entries share a
        // dependency — the engine shader headers a whole material set includes, or a single model a
        // group of meshes each extract from — so hashing per (entry, dep) re-reads the same bytes
        // repeatedly. Memoizing by path collapses that to one hash per unique file per cook, which
        // dominates the incremental (all-hit) re-cook cost. A file is static across one cook, so the
        // memo is safe within a single CookPack call.
        //
        // Workers hash their own entries' dependencies, so the map is mutex-guarded — and the hash
        // itself runs outside the lock, since serializing the read would undo the point. Two workers
        // racing on the same unseen path therefore both hash it and one insert wins; a file is
        // static across the cook, so they compute the same value and the duplicate read is the whole
        // cost of the race.
        std::mutex hashMemoMutex;
        std::unordered_map<string, ContentHash> hashMemo;
        const auto hashDep = [&hashMemoMutex, &hashMemo](const path& p) -> optional<ContentHash>
        {
            const string key = p.string();
            {
                const std::scoped_lock lock(hashMemoMutex);
                if (const auto it = hashMemo.find(key); it != hashMemo.end())
                {
                    return it->second;
                }
            }
            const optional<ContentHash> hash = HashFileContents(p);
            if (hash)
            {
                const std::scoped_lock lock(hashMemoMutex);
                hashMemo.emplace(key, *hash);
            }
            return hash;
        };

        // One blob destined for the archive: its descriptor (always known) plus, for a freshly
        // cooked blob, its bytes in hand. A cache-hit blob carries no bytes here — they are read back
        // from the cache only if the pack actually has to be written.
        struct PlannedBlob
        {
            ArchiveBlobDescriptor Descriptor;
            optional<vector<u8>> FreshBytes;
        };

        // Everything one manifest entry produces, kept in that entry's own slot rather than appended
        // to a shared container as it finishes. Two properties rest on the slots. The archive TOC is
        // laid out in manifest order whatever order the workers complete in — the cook cache's
        // unchanged-pack check hashes that TOC, so a scheduling-dependent order would make every
        // warm cook rewrite packs it did not need to. And a pack that fails on several entries at
        // once reports the first entry's error, not whichever worker happened to finish first.
        struct EntrySlot
        {
            optional<string> CacheKey;
            optional<string> Error;
            vector<PlannedBlob> Blobs;
            vector<path> Deps;
            optional<CookCacheEntry> Store;
            CookAssetTiming Timing;
            f64 CacheStoreSeconds = 0.0;
        };

        const json& assets = pack["assets"];
        vector<EntrySlot> slots(assets.size());

        // The pack is served entirely from cache only when every entry hits; one miss (or no cache)
        // means the pack is rewritten and the unchanged-pack write skip below cannot apply.
        bool allHits = cache != nullptr;

        // Phase 1, on the calling thread: every entry's cache lookup. Deliberately serial — it is
        // the only phase that touches the cook cache, so the cache itself needs no concurrency, and
        // it costs nothing to keep: a cold cook finds no entries to validate and a fully warm one
        // measures in milliseconds.
        vector<usize> misses;
        for (usize index = 0; index < assets.size(); ++index)
        {
            const json& entry = assets[index];
            EntrySlot& slot = slots[index];
            const CookStopwatch entryWatch;

            // The id and type come from the manifest entry rather than the importer table: that is
            // the pair the timing roll-up keys on, and it is available even for an entry whose type
            // never resolves to an importer.
            if (entry.is_object())
            {
                if (entry.contains("id") && entry["id"].is_string())
                {
                    if (const optional<AssetId> id = ParseAssetId(entry["id"].get<string>()))
                    {
                        slot.Timing.Id = *id;
                    }
                }
                if (entry.contains("type") && entry["type"].is_string())
                {
                    slot.Timing.Type = entry["type"].get<string>();
                }
            }

            // Compute the entry's cache key up front. A malformed entry (no key possible) simply
            // cooks fresh and reports its own located error from CookEntry.
            if (cache != nullptr && entry.is_object())
            {
                slot.CacheKey = cache->KeyFor(CookCacheKeyInputs{
                    .EntryJson = entry.dump(),
                    .PackDir = keyPackDir,
                    .ConfigFingerprint = configFingerprint,
                    .ShaderIncludeDir = keyShaderIncludeDir,
                    .ConsultsModule =
                        EntryModuleDependence(entry) == ImporterModuleDependence::DependsOnModule,
                });

                if (const optional<CookCacheMeta> meta = cache->LoadMeta(*slot.CacheKey))
                {
                    // A cache hit is trusted only if every recorded input is unchanged: each source
                    // file is unchanged (by stat, then hash), and each resolved id still maps to the
                    // same source (the id→source remap a content check alone would miss).
                    bool valid = true;
                    for (const CachedDep& dep : meta->SourceDeps)
                    {
                        const optional<FileStat> st = StatFile(dep.Path);
                        if (!st)
                        {
                            valid = false;
                            break;
                        }
                        // Fast path: an unchanged size+mtime means the file is unchanged — skip the
                        // read entirely, which is what makes an all-hit re-cook cheap. This trusts
                        // mtime for a positive match, the same assumption the build's depfile makes.
                        if (st->Size == dep.Size && st->Mtime == dep.Mtime)
                        {
                            continue;
                        }
                        // Stat differs (a touch, a checkout): fall back to the content hash. A hash
                        // that still matches is a hit, so a mtime change alone never forces a re-cook.
                        const optional<ContentHash> current = hashDep(dep.Path);
                        if (!current || current->Lo != dep.Hash.Lo || current->Hi != dep.Hash.Hi)
                        {
                            valid = false;
                            break;
                        }
                    }
                    if (valid)
                    {
                        for (const auto& [resId, resPath] : meta->Resolutions)
                        {
                            const optional<ResolvedSource> now = resolveById(resId);
                            if (!now || NormalizeDependency(now->AbsolutePath) != resPath)
                            {
                                valid = false;
                                break;
                            }
                        }
                    }

                    if (valid)
                    {
                        // Plan the stored blobs by descriptor (no bytes yet), and re-record the
                        // entry's dependencies so the depfile stays complete even though the
                        // importer never ran.
                        for (const CachedBlobMeta& blob : meta->Blobs)
                        {
                            slot.Blobs.push_back(PlannedBlob{
                                .Descriptor =
                                    ArchiveBlobDescriptor{.Id = blob.Id,
                                                          .Type = blob.Type,
                                                          .Codec = blob.Codec,
                                                          .Size = blob.Size,
                                                          .UncompressedSize = blob.UncompressedSize,
                                                          .Hash = blob.Hash},
                                .FreshBytes = std::nullopt});
                        }
                        for (const CachedDep& dep : meta->SourceDeps)
                        {
                            slot.Deps.push_back(dep.Path);
                        }
                        slot.Timing.CacheHit = true;
                        slot.Timing.CacheLookupSeconds = entryWatch.Elapsed();
                        continue;
                    }
                }
            }

            allHits = false;
            slot.Timing.CacheLookupSeconds = entryWatch.Elapsed();
            misses.push_back(index);
        }

        // The cook's one concurrency budget, split between the driver's per-asset workers and any
        // threading an importer does inside its own Cook. Splitting it rather than stacking the two
        // is the point: the texture encode already spawns workers per mip level, and a pool per
        // importer on top of a pool per asset oversubscribes every core several times over.
        const u32 budget = jobs > 0 ? jobs : std::max(1u, std::thread::hardware_concurrency());
        const u32 workers =
            static_cast<u32>(std::min<usize>(budget, std::max<usize>(misses.size(), 1)));
        // With more than one worker the budget is already spent on the asset loop, so an importer
        // encodes on its calling thread; a single-worker cook hands the whole budget inward, which
        // is what keeps a one-asset pack (or `--jobs 1` raised) as fast as it was.
        const u32 threadBudget = workers > 1 ? 1u : budget;

        // Guards every importer that has not declared itself reentrant, so at most one such Cook is
        // in flight at a time. An importer is never *assumed* safe: the default is to hold this.
        std::mutex serialLock;

        // Phase 2: cook the misses across the workers. Each entry writes only its own slot, and the
        // context it cooks under records that entry's dependencies and resolutions into the slot too
        // — nothing shared is written, so the only synchronization is the lock above.
        const auto cookOne = [&](usize index)
        {
            const json& entry = assets[index];
            EntrySlot& slot = slots[index];

            const auto entryRecord = [&slot](const path& p)
            { slot.Deps.push_back(NormalizeDependency(p)); };
            vector<std::pair<AssetId, path>> resolutions;
            const auto entryResolve = [&](AssetId id) -> optional<ResolvedSource>
            {
                const optional<ResolvedSource> resolved = resolveById(id);
                if (resolved)
                {
                    const path normalized = NormalizeDependency(resolved->AbsolutePath);
                    slot.Deps.push_back(normalized);
                    resolutions.emplace_back(id, normalized);
                }
                return resolved;
            };

            const CookContext context{
                .PackDir = packDir,
                .Resolve = entryResolve,
                .AssetTypes = &m_AssetTypes,
                .Types = types,
                .Systems = systems,
                .Config = config,
                .ShaderIncludeDir = shaderIncludeDir,
                .RecordDependency = entryRecord,
                .ThreadBudget = threadBudget,
            };

            vector<CachedBlob> blobs;
            f64 storeSeconds = 0.0;
            f64 waitSeconds = 0.0;
            const CookStopwatch cookWatch;
            const VoidResult entryResult =
                CookEntry(context, entry, serialLock, blobs, level, &storeSeconds, &waitSeconds);
            const f64 cookSeconds = cookWatch.Elapsed();
            if (!entryResult)
            {
                slot.Error = entryResult.error();
                return;
            }

            slot.Timing.SerializedWaitSeconds = waitSeconds;
            slot.Timing.ImportSeconds = cookSeconds - storeSeconds - waitSeconds;
            slot.Timing.StoreSeconds = storeSeconds;

            for (const CachedBlob& blob : blobs)
            {
                slot.Blobs.push_back(PlannedBlob{
                    .Descriptor = ArchiveBlobDescriptor{.Id = blob.Id,
                                                        .Type = blob.Type,
                                                        .Codec = blob.Codec,
                                                        .Size = blob.Bytes.size(),
                                                        .UncompressedSize = blob.UncompressedSize,
                                                        .Hash = blob.Hash},
                    .FreshBytes = blob.Bytes});
            }

            if (cache != nullptr && slot.CacheKey)
            {
                const CookStopwatch cacheWatch;
                CookCacheEntry toStore;
                toStore.Blobs = std::move(blobs);
                toStore.Resolutions = std::move(resolutions);
                // De-duplicate the entry's dependency paths, capturing a stat + content hash for
                // each; a file that cannot be read is skipped, degrading the entry to a miss next
                // time rather than being trusted.
                const std::set<path> uniqueDeps(slot.Deps.begin(), slot.Deps.end());
                for (const path& depPath : uniqueDeps)
                {
                    const optional<FileStat> st = StatFile(depPath);
                    const optional<ContentHash> hash = hashDep(depPath);
                    if (st && hash)
                    {
                        toStore.SourceDeps.push_back(CachedDep{
                            .Path = depPath, .Size = st->Size, .Mtime = st->Mtime, .Hash = *hash});
                    }
                }
                slot.Store = std::move(toStore);
                slot.CacheStoreSeconds = cacheWatch.Elapsed();
            }
        };

        RunIndexed(misses, workers, cookOne);

        // Phase 3, back on the calling thread: walk the slots in manifest order, so the first entry
        // that failed is the error reported and the TOC is laid out in manifest order.
        struct PendingStore
        {
            string Key;
            CookCacheEntry Entry;
        };
        vector<PlannedBlob> planned;
        vector<PendingStore> pendingStores;
        std::set<u64> seenIds;
        for (usize index = 0; index < slots.size(); ++index)
        {
            EntrySlot& slot = slots[index];
            if (slot.Error)
            {
                return std::unexpected(
                    fmt::format("pack '{}': asset[{}]: {}", packJson.string(), index, *slot.Error));
            }
            for (PlannedBlob& blob : slot.Blobs)
            {
                if (!seenIds.insert(blob.Descriptor.Id.Value).second)
                {
                    return std::unexpected(
                        fmt::format("pack '{}': asset[{}]: asset id {} duplicated",
                                    packJson.string(), index, blob.Descriptor.Id.Value));
                }
                planned.push_back(std::move(blob));
            }
            for (const path& dep : slot.Deps)
            {
                dependencies.insert(dep);
            }
            if (slot.Store)
            {
                pendingStores.push_back(
                    PendingStore{.Key = *slot.CacheKey, .Entry = std::move(*slot.Store)});
            }
            if (timing != nullptr)
            {
                timing->CacheStoreSeconds += slot.CacheStoreSeconds;
                timing->Assets.push_back(std::move(slot.Timing));
            }
        }
        if (timing != nullptr)
        {
            timing->Jobs = std::max(timing->Jobs, budget);
        }

        if (outDependencies)
        {
            outDependencies->assign(dependencies.begin(), dependencies.end());
        }

        // Compute the pack's identity — TOC digest + total size — from the blob descriptors alone,
        // no bytes needed. assetpack lays out the TOC exactly as it would when writing; the digest
        // (hashing lives here, not in assetpack) is the same one a full build produces.
        const CookStopwatch tocWatch;
        vector<ArchiveBlobDescriptor> descriptors;
        descriptors.reserve(planned.size());
        for (const PlannedBlob& p : planned)
        {
            descriptors.push_back(p.Descriptor);
        }
        const ArchiveTocImage toc = BuildArchiveToc(descriptors);
        const ContentHash digest = Xxh3_128(toc.TocBytes);
        if (timing != nullptr)
        {
            timing->TocDigestSeconds += tocWatch.Elapsed();
        }

        // If every entry hit and the existing pack already has this exact identity, the pack we would
        // write is byte-for-byte what is already on disk — skip reading the blobs back and skip the
        // write. This is what keeps a project cook cheap when a change to one pack forces every pack's
        // cook to re-run: an unchanged pack costs no blob reads and no write.
        if (allHits)
        {
            const optional<ArchiveIdentity> existing = ReadArchiveIdentity(outArchive);
            if (existing && existing->Digest.Lo == digest.Lo && existing->Digest.Hi == digest.Hi &&
                existing->TotalSize == toc.TotalSize)
            {
                return {};
            }
        }

        // Assemble the archive, materializing each blob's bytes: a freshly cooked blob has them in
        // hand; a cache-hit blob is read back from the cache now that the pack must be written.
        const CookStopwatch writeWatch;
        ArchiveWriter writer;
        for (PlannedBlob& p : planned)
        {
            if (p.FreshBytes)
            {
                writer.Add(p.Descriptor.Id, p.Descriptor.Type, *p.FreshBytes, p.Descriptor.Hash,
                           p.Descriptor.Codec, p.Descriptor.UncompressedSize);
                continue;
            }
            const optional<vector<u8>> bytes = cache->LoadBlob(p.Descriptor.Hash);
            if (!bytes)
            {
                return std::unexpected(
                    fmt::format("pack '{}': cached blob for asset id {} is missing from the cache",
                                packJson.string(), p.Descriptor.Id.Value));
            }
            writer.Add(p.Descriptor.Id, p.Descriptor.Type, *bytes, p.Descriptor.Hash,
                       p.Descriptor.Codec, p.Descriptor.UncompressedSize);
        }
        writer.SetArchiveDigest(digest);

        if (const VoidResult written = writer.Write(outArchive); !written)
        {
            return written;
        }
        if (timing != nullptr)
        {
            timing->ArchiveWriteSeconds += writeWatch.Elapsed();
        }

        // Persist the freshly cooked entries now that the pack is on disk.
        const CookStopwatch storeWatch;
        for (const PendingStore& ps : pendingStores)
        {
            if (const VoidResult stored = cache->Store(ps.Key, ps.Entry); !stored)
            {
                return std::unexpected(fmt::format("pack '{}': cache store failed: {}",
                                                   packJson.string(), stored.error()));
            }
        }
        if (timing != nullptr)
        {
            timing->CacheStoreSeconds += storeWatch.Elapsed();
        }

        return {};
    }

    Result<vector<u8>> Cooker::CookSource(const path& sourcePath, AssetId id, AssetTypeId type,
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
            Result<AssetPack> refPackResult = ParseAssetPack(refPath, m_AssetTypes);
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
            .AssetTypes = &m_AssetTypes,
            .Types = types,
            .Systems = systems,
            .Config = config,
            .ShaderIncludeDir = shaderIncludeDir,
            .RecordDependency = [](const path&) {},
        };

        json entry;
        entry["source"] = sourcePath.filename().string();

        const Result<vector<u8>> blob = importerIt->second.Importer->Cook(context, entry);
        if (!blob)
        {
            return std::unexpected(fmt::format("cook '{}': {}", sourcePath.string(), blob.error()));
        }

        ArchiveWriter writer;
        AddStored(writer, MakeStoredBlob(id, type, *blob, ZstdLevel));

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
                                 std::mutex& serialLock, vector<CachedBlob>& outBlobs, int level,
                                 f64* outStoreSeconds, f64* outWaitSeconds) const
    {
        // Compresses and hashes one cooked blob, accumulating that cost apart from the importer's:
        // a blob store is the same work whatever produced the bytes, so folding it into the
        // importer's duration would mis-attribute it per asset type.
        const auto storeBlob =
            [outStoreSeconds](AssetId id, AssetTypeId type, std::span<const u8> blob, int blobLevel)
        {
            const CookStopwatch watch;
            CachedBlob stored = MakeStoredBlob(id, type, blob, blobLevel);
            if (outStoreSeconds != nullptr)
            {
                *outStoreSeconds += watch.Elapsed();
            }
            return stored;
        };

        if (!entry.is_object())
        {
            return std::unexpected("entry is not an object");
        }

        if (!entry.contains("id") || !entry["id"].is_string())
        {
            return std::unexpected("missing or invalid 'id' (expected a hex id string)");
        }

        const optional<AssetId> parsedId = ParseAssetId(entry["id"].get<string>());
        if (!parsedId)
        {
            return std::unexpected(fmt::format("malformed hex id '{}'", entry["id"].get<string>()));
        }

        const u64 id = parsedId->Value;
        if (id == 0)
        {
            return std::unexpected("asset id 0 is reserved (invalid AssetId)");
        }

        if (!entry.contains("type") || !entry["type"].is_string())
        {
            return std::unexpected("missing or invalid 'type'");
        }

        const string typeStr = entry["type"].get<string>();
        const optional<AssetTypeId> type = m_AssetTypes.FindByName(typeStr);
        if (!type)
        {
            return std::unexpected(UnknownTypeReason(typeStr, m_AssetTypes));
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

        // An importer that has not declared itself reentrant runs under the cook's serialization
        // lock — held for the rest of the entry, so a parent Material's companion instance cook is
        // covered too. An importer is never assumed safe; declaring nothing means holding this.
        const AssetImporter& importer = *importerIt->second.Importer;
        std::unique_lock<std::mutex> serialize(serialLock, std::defer_lock);
        if (importer.Concurrency() != ImporterConcurrency::Parallel)
        {
            const CookStopwatch waitWatch;
            serialize.lock();
            if (outWaitSeconds != nullptr)
            {
                *outWaitSeconds += waitWatch.Elapsed();
            }
        }

        const Result<vector<u8>> blob = importer.Cook(context, entry);
        if (!blob)
        {
            return std::unexpected(blob.error());
        }

        outBlobs.push_back(storeBlob(AssetId{.Value = id}, *type, *blob, level));

        // A parent Material whose `*.vmat.json` declares a `defaultInstance` id emits a companion
        // zero-override MaterialInstance at that id, so every direct reference names a real instance
        // archive entry. The id lives in the material source, not the pack manifest, so the material
        // editor mints and writes it through the same `.vmat` round-trip it already owns.
        if (*type == AssetTypes::Material && entry.contains("source") &&
            entry["source"].is_string())
        {
            const path vmatPath = context.PackDir / entry["source"].get<string>();
            const std::ifstream vmatFile(vmatPath, std::ios::binary);
            std::ostringstream vmatContent;
            vmatContent << vmatFile.rdbuf();
            const json vmat = json::parse(vmatContent.str(), nullptr, false);
            if (vmat.is_discarded() || !vmat.is_object())
            {
                return std::unexpected(fmt::format("material source '{}' is not a JSON object",
                                                   entry["source"].get<string>()));
            }

            if (vmat.contains("defaultInstance"))
            {
                if (!vmat["defaultInstance"].is_string())
                {
                    return std::unexpected("'defaultInstance' must be a hex id string");
                }
                const optional<AssetId> parsedDefault =
                    ParseAssetId(vmat["defaultInstance"].get<string>());
                if (!parsedDefault)
                {
                    return std::unexpected(
                        fmt::format("'defaultInstance' is a malformed hex id '{}'",
                                    vmat["defaultInstance"].get<string>()));
                }
                const u64 defaultInstanceId = parsedDefault->Value;
                if (defaultInstanceId == 0)
                {
                    return std::unexpected("'defaultInstance' id 0 is reserved (invalid AssetId)");
                }
                const Result<vector<u8>> instanceBlob = CookDefaultInstanceBlob(context, id);
                if (!instanceBlob)
                {
                    return std::unexpected(instanceBlob.error());
                }

                outBlobs.push_back(storeBlob(AssetId{.Value = defaultInstanceId},
                                             AssetTypes::MaterialInstance, *instanceBlob, level));
            }
        }

        return {};
    }
}
