#include <Veng/Cook/CookCache.h>

#include <filesystem>
#include <fstream>
#include <iterator>
#include <string_view>

#include <fmt/format.h>
#include <xxhash.h>

#include <Veng/Asset/AtomicFile.h>
#include <Veng/Asset/HexId.h>
#include <Veng/Cook/JsonFile.h>
#include <Veng/Project/CompressionRole.h>

namespace Veng::Cook
{
    namespace
    {
        // The cache's own format version. Folded into the tool tag so a change to the cache file
        // layout or the manifest schema invalidates every cached entry without a manual sweep.
        constexpr u32 CookCacheFormatVersion = 3;

        // Renders a 128-bit hash as a fixed 32-char lowercase hex string, the basename form used
        // for both cache keys and content-addressed blob files.
        string HexOf(ContentHash h)
        {
            return fmt::format("{:016x}{:016x}", h.Hi, h.Lo);
        }

        // Reads a whole file into bytes with a single bulk read; false on failure. A stream_size /
        // resize / read() beats an istreambuf_iterator fill by a wide margin — the iterator copies a
        // character at a time, which dominates the warm-cook wall clock when every cached blob and
        // dependency is read back through this path.
        bool ReadFileBytes(const path& file, vector<u8>& out)
        {
            std::ifstream in(file, std::ios::binary | std::ios::ate);
            if (!in)
            {
                return false;
            }
            const std::streamoff size = in.tellg();
            if (size < 0)
            {
                return false;
            }
            out.resize(static_cast<usize>(size));
            if (size == 0)
            {
                return true;
            }
            in.seekg(0);
            in.read(reinterpret_cast<char*>(out.data()), size);
            return static_cast<bool>(in);
        }
    }

    optional<FileStat> StatFile(const path& file)
    {
        std::error_code ec;
        const auto size = std::filesystem::file_size(file, ec);
        if (ec)
        {
            return std::nullopt;
        }
        const auto mtime = std::filesystem::last_write_time(file, ec);
        if (ec)
        {
            return std::nullopt;
        }
        return FileStat{.Size = static_cast<u64>(size),
                        .Mtime = static_cast<i64>(mtime.time_since_epoch().count())};
    }

    optional<ContentHash> HashFileContents(const path& file)
    {
        vector<u8> bytes;
        if (!ReadFileBytes(file, bytes))
        {
            return std::nullopt;
        }
        const XXH128_hash_t h = XXH3_128bits(bytes.data(), bytes.size());
        return ContentHash{.Lo = h.low64, .Hi = h.high64};
    }

    string FingerprintBuildConfiguration(const BuildConfiguration& config)
    {
        // Every field that steers a stored blob's bytes, serialized deterministically. The role
        // table is walked in the fixed CompressionRoles order and each format written by its
        // ordinal — reorderings of the enum never silently alias two tables because the whole
        // fingerprint (and the cache) is invalidated by the tool tag on any cooker rebuild.
        string out = fmt::format("name={};target={};level={};", config.Name, config.Target,
                                 config.CompressionLevel);
        for (const CompressionRole role : CompressionRoles)
        {
            out += fmt::format("{}={};", ToString(role),
                               static_cast<u32>(config.Formats.GetFormat(role)));
        }
        return out;
    }

    string ComputeCookToolTag(const path& toolExe, const path& modulePath,
                              const path& cookModulePath)
    {
        // Each image contributes `<label>=<path>` plus, when it can be stat'd, its size and mtime.
        // The path is emitted even for an unstattable image so it never drops out of the tag
        // silently — two cooks differing only in which module they loaded must still key apart.
        const auto append = [](string& tag, std::string_view label, const path& file)
        {
            if (file.empty())
            {
                return;
            }
            std::error_code ec;
            const path canonical = std::filesystem::weakly_canonical(file, ec);
            const path& resolved = ec ? file : canonical;
            tag += fmt::format(";{}={}", label, resolved.string());
            if (const optional<FileStat> stat = StatFile(resolved))
            {
                tag +=
                    fmt::format(";{}_size={};{}_mtime={}", label, stat->Size, label, stat->Mtime);
            }
        };

        string tag = fmt::format("cachefmt={}", CookCacheFormatVersion);
        append(tag, "exe", toolExe);
        append(tag, "module", modulePath);
        append(tag, "cookmodule", cookModulePath);
        return tag;
    }

    Result<CookCache> CookCache::Open(const path& cacheDir, string toolTag)
    {
        CookCache cache;
        cache.m_Root = cacheDir;
        cache.m_EntriesDir = cacheDir / "entries";
        cache.m_BlobsDir = cacheDir / "blobs";
        cache.m_ToolTag = std::move(toolTag);

        std::error_code ec;
        std::filesystem::create_directories(cache.m_EntriesDir, ec);
        if (ec)
        {
            return std::unexpected(fmt::format("cache '{}': cannot create entries dir: {}",
                                               cacheDir.string(), ec.message()));
        }
        std::filesystem::create_directories(cache.m_BlobsDir, ec);
        if (ec)
        {
            return std::unexpected(fmt::format("cache '{}': cannot create blobs dir: {}",
                                               cacheDir.string(), ec.message()));
        }
        return cache;
    }

    string CookCache::KeyFor(const CookCacheKeyInputs& inputs) const
    {
        // Concatenate every keying input behind length-tagged separators so no two distinct field
        // splits ever hash equal, then xxh3-128 the whole and render it hex.
        const string material = fmt::format(
            "tool={}\nentry={}\npackdir={}\nconfig={}\nshaderinc={}\n", m_ToolTag, inputs.EntryJson,
            inputs.PackDir.string(), inputs.ConfigFingerprint, inputs.ShaderIncludeDir.string());
        const XXH128_hash_t h = XXH3_128bits(material.data(), material.size());
        return HexOf(ContentHash{.Lo = h.low64, .Hi = h.high64});
    }

    optional<CookCacheMeta> CookCache::LoadMeta(const string& key) const
    {
        const path manifestPath = m_EntriesDir / (key + ".json");
        std::error_code ec;
        if (!std::filesystem::exists(manifestPath, ec))
        {
            return std::nullopt;
        }

        const Result<json> manifestResult = ReadJsonFile(manifestPath, "cache");
        if (!manifestResult)
        {
            // A corrupt manifest is a miss, not an error — the cook re-cooks and overwrites it.
            return std::nullopt;
        }
        const json& manifest = *manifestResult;
        if (!manifest.contains("version") || !manifest["version"].is_number_unsigned() ||
            manifest["version"].get<u64>() != 2)
        {
            return std::nullopt;
        }

        CookCacheMeta meta;

        if (manifest.contains("deps") && manifest["deps"].is_array())
        {
            for (const json& dep : manifest["deps"])
            {
                if (!dep.is_object() || !dep.contains("path") || !dep["path"].is_string() ||
                    !dep.contains("size") || !dep.contains("mtime") || !dep.contains("lo") ||
                    !dep.contains("hi"))
                {
                    return std::nullopt;
                }
                meta.SourceDeps.push_back(CachedDep{
                    .Path = path(dep["path"].get<string>()),
                    .Size = dep["size"].get<u64>(),
                    .Mtime = dep["mtime"].get<i64>(),
                    .Hash = ContentHash{.Lo = dep["lo"].get<u64>(), .Hi = dep["hi"].get<u64>()},
                });
            }
        }

        if (manifest.contains("resolutions") && manifest["resolutions"].is_array())
        {
            for (const json& res : manifest["resolutions"])
            {
                if (!res.is_object() || !res.contains("id") || !res["id"].is_string() ||
                    !res.contains("path") || !res["path"].is_string())
                {
                    return std::nullopt;
                }
                const optional<AssetId> id = ParseAssetId(res["id"].get<string>());
                if (!id)
                {
                    return std::nullopt;
                }
                meta.Resolutions.emplace_back(*id, path(res["path"].get<string>()));
            }
        }

        if (!manifest.contains("blobs") || !manifest["blobs"].is_array())
        {
            return std::nullopt;
        }
        for (const json& blob : manifest["blobs"])
        {
            if (!blob.is_object() || !blob.contains("id") || !blob["id"].is_string() ||
                !blob.contains("type") || !blob.contains("codec") || !blob.contains("size") ||
                !blob.contains("usize") || !blob.contains("lo") || !blob.contains("hi"))
            {
                return std::nullopt;
            }
            const optional<AssetId> id = ParseAssetId(blob["id"].get<string>());
            if (!id)
            {
                return std::nullopt;
            }
            if (!blob["type"].is_string() || !ParseHexId(blob["type"].get<string>()))
            {
                return std::nullopt;
            }
            meta.Blobs.push_back(CachedBlobMeta{
                .Id = *id,
                .Type = AssetTypeId{.Value = *ParseHexId(blob["type"].get<string>())},
                .Codec = static_cast<ArchiveCodec>(blob["codec"].get<u32>()),
                .Size = blob["size"].get<u64>(),
                .UncompressedSize = blob["usize"].get<u64>(),
                .Hash = ContentHash{.Lo = blob["lo"].get<u64>(), .Hi = blob["hi"].get<u64>()},
            });
        }

        return meta;
    }

    optional<vector<u8>> CookCache::LoadBlob(ContentHash hash) const
    {
        vector<u8> bytes;
        if (!ReadFileBytes(m_BlobsDir / (HexOf(hash) + ".blob"), bytes))
        {
            return std::nullopt;
        }
        return bytes;
    }

    VoidResult CookCache::Store(const string& key, const CookCacheEntry& entry) const
    {
        // Content-addressed blob files first, so a manifest never references a missing blob.
        for (const CachedBlob& blob : entry.Blobs)
        {
            const path blobPath = m_BlobsDir / (HexOf(blob.Hash) + ".blob");
            std::error_code ec;
            if (std::filesystem::exists(blobPath, ec))
            {
                // Identical stored bytes are already present (same hash → same content); skip.
                continue;
            }
            if (const VoidResult written = WriteFileAtomic(blobPath, blob.Bytes); !written)
            {
                return std::unexpected(written.error());
            }
        }

        json manifest;
        manifest["version"] = 2;

        json deps = json::array();
        for (const CachedDep& d : entry.SourceDeps)
        {
            json dep;
            dep["path"] = d.Path.string();
            dep["size"] = d.Size;
            dep["mtime"] = d.Mtime;
            dep["lo"] = d.Hash.Lo;
            dep["hi"] = d.Hash.Hi;
            deps.push_back(std::move(dep));
        }
        manifest["deps"] = std::move(deps);

        json resolutions = json::array();
        for (const auto& [id, resPath] : entry.Resolutions)
        {
            json res;
            res["id"] = FormatAssetId(id);
            res["path"] = resPath.string();
            resolutions.push_back(std::move(res));
        }
        manifest["resolutions"] = std::move(resolutions);

        json blobs = json::array();
        for (const CachedBlob& blob : entry.Blobs)
        {
            json b;
            b["id"] = FormatAssetId(blob.Id);
            b["type"] = FormatHexId(blob.Type.Value);
            b["codec"] = static_cast<u32>(blob.Codec);
            b["size"] = blob.Bytes.size();
            b["usize"] = blob.UncompressedSize;
            b["lo"] = blob.Hash.Lo;
            b["hi"] = blob.Hash.Hi;
            blobs.push_back(std::move(b));
        }
        manifest["blobs"] = std::move(blobs);

        const string text = manifest.dump();
        const path manifestPath = m_EntriesDir / (key + ".json");
        return WriteFileAtomic(manifestPath,
                               std::span(reinterpret_cast<const u8*>(text.data()), text.size()));
    }
}
