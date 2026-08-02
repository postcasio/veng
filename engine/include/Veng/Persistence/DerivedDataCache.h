#pragma once

#include <span>

#include <Veng/Path.h>
#include <Veng/Result.h>
#include <Veng/Veng.h>

namespace Veng
{
    /// @brief What a cache is holding and how it has been used since it was opened.
    struct DerivedDataCacheStats
    {
        /// @brief Entries currently indexed.
        usize Entries = 0;
        /// @brief Total payload bytes across the indexed entries.
        u64 Bytes = 0;
        /// @brief Reads that found a usable entry.
        u64 Hits = 0;
        /// @brief Reads that found nothing, or found something unusable.
        u64 Misses = 0;
        /// @brief Entries written.
        u64 Stores = 0;
        /// @brief Entries dropped to stay within the caps.
        u64 Evictions = 0;
    };

    /// @brief How a cache is opened: where it lives, what generation it holds, and its caps.
    struct DerivedDataCacheInfo
    {
        /// @brief The directory the cache owns its files within; created when absent.
        ///
        /// UserCacheDir() is the natural provider — the platform's expendable-content base, whose
        /// contract ("a caller may not assume it survives across runs") is exactly this cache's.
        path Root;

        /// @brief The one generation the cache holds, composed by the caller and opaque here.
        ///
        /// A cache whose recorded generation differs from this one is wiped at Open, which is the
        /// whole invalidation model: coarse, and so free of the failure where a fine-grained key
        /// forgets one of its inputs. What belongs in a generation is the caller's problem; the two
        /// standard ingredients are the content digests of the archives the caller has mounted
        /// (`ReadArchiveIdentity`, `Veng/Asset/Archive.h`) and a caller-owned version constant
        /// covering the inputs that live outside any archive — a derivation whose code changed
        /// moves every entry without moving a single archive byte.
        string Generation;

        /// @brief The most entries the cache holds; the oldest-touched are evicted past it.
        u64 MaxEntries = 1024;

        /// @brief The most payload bytes the cache holds; the oldest-touched are evicted past it.
        u64 MaxBytes = 512ull * 1024ull * 1024ull;
    };

    /// @brief A generation-keyed, LRU-capped store of expendable derived blobs on disk.
    ///
    /// An entry is `(generation, key, blob)`. Both strings are caller-composed and opaque: the
    /// cache never interprets a key and never knows what a blob holds. It exists so work that is a
    /// pure function of the caller's inputs — a baked texture, a prefiltered environment, a
    /// computed lookup table — survives a restart instead of being recomputed on every run.
    ///
    /// **The cache is never a source of truth.** A deleted directory, a truncated blob, a flipped
    /// byte and a cap-driven eviction are all the same outcome: the read misses and the caller does
    /// the work it would have done had the cache never existed. Nothing here fails harder than
    /// that, so no caller needs a recovery path.
    ///
    /// **Invalidation is whole-cache.** Open with a generation differing from the recorded one and
    /// every entry is deleted before the first read; there is no per-entry validity question to get
    /// wrong.
    ///
    /// **The caps are LRU over both count and bytes**, evaluated on insert, evicting
    /// oldest-touched first; a successful read touches. The index is rewritten atomically
    /// (temp-then-rename) and each blob is one file written the same way, so an interrupted write
    /// leaves either the prior state or a stray temp file the next Open sweeps.
    ///
    /// **Within its root the cache owns the `cache.` file-name prefix and nothing else.** The index
    /// and every blob carry it, eviction deletes only through the index, and a wipe removes only
    /// files matching that prefix — a root shared with other content keeps it.
    ///
    /// Every method is safe to call from any thread: a mutex covers the index and the file I/O
    /// alike, so several task-system workers may probe and store concurrently. That is the intended
    /// usage — file I/O belongs on a worker, never on the render thread.
    class VE_API DerivedDataCache
    {
    public:
        /// @brief The file-name prefix every file the cache writes carries.
        static constexpr string_view FilePrefix = "cache.";

        /// @brief Opens a cache on a directory, wiping it when the recorded generation differs.
        ///
        /// Creates the directory when absent, reads the index, drops entries whose blob file has
        /// gone missing, and sweeps stray temp files from an interrupted write. An index the cache
        /// cannot read is treated as an empty cache and replaced, since an unreadable cache and a
        /// cold one call for exactly the same behavior.
        /// @param info  The root, the generation, and the caps.
        /// @return The opened cache, or an error when the root cannot be created.
        [[nodiscard]] static Result<Unique<DerivedDataCache>>
        Open(const DerivedDataCacheInfo& info);

        /// @brief Closes the cache; the index on disk already reflects every completed store.
        ~DerivedDataCache();

        DerivedDataCache(const DerivedDataCache&) = delete;
        DerivedDataCache& operator=(const DerivedDataCache&) = delete;

        /// @brief Reads an entry's payload, touching it so it evicts last.
        ///
        /// A missing, short, or digest-mismatched blob reads as a miss and its file and index entry
        /// are removed, so a corrupted entry costs one recomputation and is not met twice.
        /// @param key  The caller's key.
        /// @return The stored payload, or nullopt on a miss.
        [[nodiscard]] optional<vector<u8>> Read(string_view key);

        /// @brief Writes an entry, replacing any entry already under the key, then applies the caps.
        ///
        /// The blob is written to a temp file and renamed into place, and the index is rewritten
        /// the same way, so a crash mid-store leaves the cache readable at its prior state.
        /// @param key      The caller's key.
        /// @param payload  The bytes to store; kept verbatim.
        /// @return True when the entry was written; false when the write failed (the cache is
        ///         unchanged, and the caller has lost nothing but the saving).
        bool Store(string_view key, std::span<const u8> payload);

        /// @brief Removes an entry and its file.
        /// @param key  The caller's key.
        /// @return True when an entry was removed.
        bool Erase(string_view key);

        /// @brief Removes every entry and every file the cache owns within its root.
        void Clear();

        /// @brief Returns whether a key names an indexed entry, without touching it.
        ///
        /// Answers the index alone — it does not open the blob, so a corrupted entry still reports
        /// true here and misses on Read.
        /// @param key  The caller's key.
        [[nodiscard]] bool Contains(string_view key) const;

        /// @brief Returns what the cache is holding and how it has been used.
        [[nodiscard]] DerivedDataCacheStats GetStats() const;

        /// @brief Returns the directory the cache owns its files within.
        [[nodiscard]] const path& GetRoot() const;

        /// @brief Returns the generation every entry in the cache was written under.
        [[nodiscard]] const string& GetGeneration() const;

    private:
        struct State;

        explicit DerivedDataCache(Unique<State> state);

        /// @brief The index, the caps, the counters, and the mutex covering them.
        Unique<State> m_State;
    };
}
