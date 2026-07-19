#pragma once

#include <Veng/Asset/Archive.h>
#include <Veng/Asset/AssetId.h>
#include <Veng/Asset/AssetType.h>
#include <Veng/Cook/Types.h>
#include <Veng/Project/BuildConfiguration.h>

#include <span>
#include <utility>

namespace Veng::Cook
{
    /// @brief A single stored blob exactly as it lands in a .vengpack archive.
    ///
    /// The "final compressed binary format" the cache preserves: the bytes are the stored
    /// (zstd-compressed or raw) form already chosen by the cook, together with the codec, the
    /// inflated length, and the content hash of the stored bytes. Replaying a cache hit means
    /// handing these straight to ArchiveWriter::Add — no importer run, no re-compression.
    struct CachedBlob
    {
        /// @brief The asset identifier this blob is stored under.
        AssetId Id;
        /// @brief The asset type of the blob.
        AssetTypeId Type{};
        /// @brief How the stored bytes are encoded (Stored or Zstd).
        ArchiveCodec Codec = ArchiveCodec::Stored;
        /// @brief The inflated blob length; equals the stored size for a Stored blob.
        u64 UncompressedSize = 0;
        /// @brief xxh3-128 content hash of the stored bytes; also the blob's content-address in the cache.
        ContentHash Hash;
        /// @brief The stored bytes (the exact bytes ArchiveWriter::Add receives).
        vector<u8> Bytes;
    };

    /// @brief A stored blob's descriptor, without its bytes.
    ///
    /// Enough to lay out the archive TOC and compute its digest (via ArchiveBlobDescriptor), so an
    /// unchanged pack is recognized from cached metadata without reading any blob file.
    struct CachedBlobMeta
    {
        /// @brief The asset identifier this blob is stored under.
        AssetId Id;
        /// @brief The asset type of the blob.
        AssetTypeId Type{};
        /// @brief How the stored bytes are encoded (Stored or Zstd).
        ArchiveCodec Codec = ArchiveCodec::Stored;
        /// @brief The stored (on-disk) byte length of the blob.
        u64 Size = 0;
        /// @brief The inflated blob length; equals Size for a Stored blob.
        u64 UncompressedSize = 0;
        /// @brief xxh3-128 content hash of the stored bytes; also the blob's content-address.
        ContentHash Hash;
    };

    /// @brief One recorded source dependency: its path, a size+mtime stat, and a content hash.
    ///
    /// The size+mtime let a later cook confirm the file is unchanged with a stat alone (no read) —
    /// the fast path that dominates an incremental re-cook. The content hash is the source of truth:
    /// when the stat differs (a touch, a branch switch), the file is re-hashed and a matching hash is
    /// still a hit, so a mtime change without a content change never forces a re-cook. Content that
    /// changes while size and mtime both stay identical is the one case the stat fast-path trusts —
    /// the same assumption the build's own depfile makes when it decides whether to run the cook.
    struct CachedDep
    {
        /// @brief Absolute, normalized path of the dependency file.
        path Path;
        /// @brief File size in bytes at cook time; the first half of the stat fast-path check.
        u64 Size = 0;
        /// @brief Last-write time as a filesystem-clock tick count; the second half of the check.
        i64 Mtime = 0;
        /// @brief xxh3-128 of the file's contents at cook time; the authoritative unchanged check.
        ContentHash Hash;
    };

    /// @brief A file's size and last-write time, for the stat fast-path.
    struct FileStat
    {
        /// @brief File size in bytes.
        u64 Size = 0;
        /// @brief Last-write time as a filesystem-clock tick count.
        i64 Mtime = 0;
    };

    /// @brief Stats a file for its size and last-write time, or nullopt if it cannot be stat'd.
    ///
    /// The Mtime is the raw filesystem-clock tick count; it is only ever compared against another
    /// tick count recorded on the same machine, never interpreted as a wall-clock time.
    /// @param file  The file to stat.
    /// @return The size and mtime, or nullopt when the file is missing/unreadable.
    [[nodiscard]] optional<FileStat> StatFile(const path& file);

    /// @brief The full cooked result of one manifest entry, plus the inputs that determined it.
    ///
    /// The store form: Blobs carry their bytes so Store can write them. One entry can emit more than
    /// one blob (a parent Material also emits its default MaterialInstance), so Blobs is a list.
    /// SourceDeps and Resolutions are the recorded inputs the cook read.
    struct CookCacheEntry
    {
        /// @brief Every stored blob the entry emitted, in emit order.
        vector<CachedBlob> Blobs;
        /// @brief Source files the entry read, each with a stat and content hash captured at cook time.
        vector<CachedDep> SourceDeps;
        /// @brief Cross-asset ids the entry resolved, each with the source path it resolved to.
        vector<std::pair<AssetId, path>> Resolutions;
    };

    /// @brief A cache entry's metadata, without any blob bytes.
    ///
    /// The load form used to validate a hit and lay out the archive: the blob descriptors let the
    /// cook compute the pack's digest without reading a byte, and the deps/resolutions gate the hit.
    /// A hit whose pack turns out unchanged is served entirely from this — no blob file is read.
    /// The cache is valid for a later cook only while every SourceDeps file is unchanged (by stat,
    /// then hash) and every Resolutions id still maps to the same source — the same invalidation the
    /// depfile encodes, plus the id→source mapping check a depfile cannot express.
    struct CookCacheMeta
    {
        /// @brief Descriptors of the blobs the entry emitted, in emit order (no bytes).
        vector<CachedBlobMeta> Blobs;
        /// @brief Source files the entry read, each with a stat and content hash captured at cook time.
        vector<CachedDep> SourceDeps;
        /// @brief Cross-asset ids the entry resolved, each with the source path it resolved to.
        vector<std::pair<AssetId, path>> Resolutions;
    };

    /// @brief The per-entry inputs, all known before cooking, that select a cache entry.
    ///
    /// These are only half the key: the tool tag (see ComputeCookToolTag) fingerprints the code
    /// that does the cooking, and these fields the data it cooks. Two cooks agreeing on both
    /// produce byte-identical stored blobs by construction, so between them they fold everything
    /// that steers a blob's bytes: the cooker executable and both dlopened module images, plus the
    /// entry's manifest JSON (id, type, per-asset fields), the pack directory (source paths are
    /// relative to it, so it disambiguates two packs that reuse a relative source name), the active
    /// configuration's fingerprint (role → format table + zstd level, so macOS/Windows/debug/release
    /// never collide), and the engine shader-include directory (it changes which engine header a
    /// shader resolves).
    struct CookCacheKeyInputs
    {
        /// @brief The manifest entry JSON, canonically serialized (id, type, source, per-asset fields).
        string EntryJson;
        /// @brief The pack directory; entry source paths resolve against it, so it separates packs.
        path PackDir;
        /// @brief The active configuration's fingerprint, or empty for a zero-config cook.
        string ConfigFingerprint;
        /// @brief The engine core shader-include directory threaded onto the cook, or empty.
        path ShaderIncludeDir;
    };

    /// @brief xxh3-128 of a file's contents, or nullopt if the file cannot be read.
    ///
    /// Used both to fingerprint a dependency when storing a cache entry and to check it is unchanged
    /// on a later lookup. A missing file yields nullopt, which a lookup treats as a cache miss.
    /// @param file  The file to hash.
    /// @return The content hash, or nullopt when the file cannot be opened.
    [[nodiscard]] optional<ContentHash> HashFileContents(const path& file);

    /// @brief A stable fingerprint of the parts of a build configuration that steer a cooked blob.
    ///
    /// Folds the name, target, zstd compression level, and the full role → format table, all by
    /// value/name, into a deterministic string. Two configurations with the same fingerprint cook
    /// every blob identically; a different codec table or compression level yields a different
    /// fingerprint and so a separate cache lineage.
    /// @param config  The configuration to fingerprint.
    /// @return The deterministic fingerprint string.
    [[nodiscard]] string FingerprintBuildConfiguration(const BuildConfiguration& config);

    /// @brief Fingerprints every image a cook runs importer code from, for CookCache::Open.
    ///
    /// A blob's bytes are decided by code, not only by data: the cooker executable, the runtime
    /// module whose reflected field layouts the prefab/level/table encoders walk, and the cook
    /// module supplying a game type's importer. Rebuilding any of them can change what an unchanged
    /// source cooks to, so all three are folded into the tag that keys every entry, alongside the
    /// cache-format version. The fingerprint is path + size + mtime per image; the size and mtime
    /// are dropped for an image that cannot be stat'd, leaving its path, and the format version is
    /// the one component always present.
    ///
    /// The tag is deliberately coarse — any module rebuild invalidates every entry, matching how a
    /// vengc rebuild already behaves. The cooker cannot tell which importer came from which image,
    /// so a finer per-entry dependency would have to guess, and a wrong fine-grained key is worse
    /// than a right coarse one.
    /// @param toolExe         The cook tool's own executable, or empty when it cannot be located.
    /// @param modulePath      The --module runtime module, or empty when none is loaded.
    /// @param cookModulePath  The resolved cook module, or empty when none is loaded.
    /// @return The tool identity string to open a cache with.
    [[nodiscard]] string ComputeCookToolTag(const path& toolExe, const path& modulePath,
                                            const path& cookModulePath);

    /// @brief A content-addressed on-disk cache of cooked, compression-ready asset blobs.
    ///
    /// Lets a re-cook skip the expensive importer + compression work for any asset whose inputs are
    /// unchanged: the stored blobs are read back and handed to ArchiveWriter directly. The store is
    /// two-level under one directory: `entries/<key>.json` records a cook's dependency + resolution
    /// manifest and the descriptors of the blobs it produced; `blobs/<hash>.blob` holds each blob's
    /// stored bytes, content-addressed so identical outputs across entries or configurations share
    /// one file. Every write is atomic, so a killed cook never strands a torn cache file.
    ///
    /// The cache is a pure optimization: a miss (or any validation failure) simply re-cooks. It is
    /// never consulted for correctness — the runtime never reads it, and vengc verify ignores it.
    class CookCache
    {
    public:
        /// @brief Opens (creating if absent) a cache rooted at @p cacheDir.
        ///
        /// @p toolTag folds into every key, so a rebuild of any image the cook runs code from — or a
        /// change to the cache format — invalidates the whole cache without a manual sweep. The
        /// caller builds it with ComputeCookToolTag.
        /// @param cacheDir  The cache root directory; its `entries/` and `blobs/` subdirs are created.
        /// @param toolTag   A cook-tool identity string mixed into every cache key.
        /// @return The opened cache, or a located error if the directories cannot be created.
        [[nodiscard]] static Result<CookCache> Open(const path& cacheDir, string toolTag);

        /// @brief Computes the hex cache key for a manifest entry's key inputs.
        ///
        /// Folds the tool tag with every field of @p inputs into an xxh3-128, rendered as a 32-char
        /// hex string — the basename of the entry's `entries/<key>.json` file.
        /// @param inputs  The pre-cook inputs that select the entry.
        /// @return The hex cache key.
        [[nodiscard]] string KeyFor(const CookCacheKeyInputs& inputs) const;

        /// @brief Loads the metadata for @p key — blob descriptors, deps, and resolutions, no bytes.
        ///
        /// Reads only the small manifest JSON, so a hit whose pack is unchanged costs no blob reads.
        /// Returns nullopt on a cold key or an unreadable/parse-failed manifest, which the caller
        /// treats as a miss. The result is unvalidated: the caller must confirm each SourceDeps file
        /// is unchanged and each Resolutions id still maps to the same source before trusting it.
        /// @param key  The hex cache key from KeyFor.
        /// @return The stored metadata, or nullopt if absent or unusable.
        [[nodiscard]] optional<CookCacheMeta> LoadMeta(const string& key) const;

        /// @brief Reads back one stored blob's bytes by its content hash.
        ///
        /// Used to assemble the archive when a pack must actually be written (some entry changed, or
        /// the existing pack does not match); an unchanged pack never calls this.
        /// @param hash  The blob's xxh3-128 content hash (its content-address in the cache).
        /// @return The stored bytes, or nullopt if the blob file is missing/unreadable.
        [[nodiscard]] optional<vector<u8>> LoadBlob(ContentHash hash) const;

        /// @brief Stores a freshly cooked entry under @p key.
        ///
        /// Writes each blob's bytes to its content-addressed `blobs/<hash>.blob` (skipping one that
        /// already exists) and the manifest to `entries/<key>.json`, all atomically. A store failure
        /// is returned as an error but is never fatal to a cook — a caller may log and continue,
        /// since the cache is optional.
        /// @param key    The hex cache key from KeyFor.
        /// @param entry  The cooked entry to persist.
        /// @return An error string on a write failure.
        [[nodiscard]] VoidResult Store(const string& key, const CookCacheEntry& entry) const;

    private:
        CookCache() = default;

        /// @brief The cache root directory.
        path m_Root;
        /// @brief The `entries/` subdirectory holding per-key manifest JSONs.
        path m_EntriesDir;
        /// @brief The `blobs/` subdirectory holding content-addressed stored blobs.
        path m_BlobsDir;
        /// @brief The cook-tool identity mixed into every key.
        string m_ToolTag;
    };
}
