#include <Veng/Persistence/DerivedDataCache.h>

#include <algorithm>
#include <array>
#include <charconv>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <mutex>
#include <system_error>
#include <unordered_map>
#include <unordered_set>

#include <zlib.h>

#include <fmt/format.h>

#include <Veng/Log.h>

namespace Veng
{
    namespace
    {
        // The index file's name within the root, and the extensions of the two file kinds beside it.
        constexpr string_view IndexFileName = "cache.index";
        constexpr string_view BlobExtension = ".blob";
        constexpr string_view TempExtension = ".tmp";

        // On-disk identities. Both are checked before a byte is trusted; a mismatch reads as
        // "not ours", which for a cache is the same outcome as "not there".
        constexpr std::array<u8, 8> IndexMagic = {'V', 'N', 'G', '.', 'D', 'D', 'C', '1'};
        constexpr std::array<u8, 8> BlobMagic = {'V', 'N', 'G', '.', 'D', 'D', 'B', '1'};
        constexpr u32 FormatVersion = 1;

        // Bounds on what an index file may claim, so a corrupt header cannot drive an allocation.
        // The index the cache writes is bounded by the caps, which are far below these.
        constexpr u32 MaxIndexEntries = 1u << 20;
        constexpr u32 MaxKeyLength = 4096;

        // --- little-endian scalar codec ---------------------------------------------------------

        template <typename T>
        void Put(vector<u8>& out, const T value)
        {
            for (usize i = 0; i < sizeof(T); i++)
            {
                out.push_back(static_cast<u8>((static_cast<u64>(value) >> (i * 8)) & 0xFFu));
            }
        }

        void PutBytes(vector<u8>& out, const string_view text)
        {
            out.insert(out.end(), text.begin(), text.end());
        }

        // A cursor over a byte buffer that reports exhaustion rather than reading past the end.
        struct Reader
        {
            std::span<const u8> Bytes;
            usize Offset = 0;

            template <typename T>
            [[nodiscard]] bool Take(T& value)
            {
                if (Bytes.size() - Offset < sizeof(T))
                {
                    return false;
                }
                u64 raw = 0;
                for (usize i = 0; i < sizeof(T); i++)
                {
                    raw |= static_cast<u64>(Bytes[Offset + i]) << (i * 8);
                }
                Offset += sizeof(T);
                value = static_cast<T>(raw);
                return true;
            }

            [[nodiscard]] bool TakeString(const u32 length, string& value)
            {
                if (Bytes.size() - Offset < length)
                {
                    return false;
                }
                value.assign(reinterpret_cast<const char*>(Bytes.data() + Offset), length);
                Offset += length;
                return true;
            }

            [[nodiscard]] bool TakeMagic(const std::array<u8, 8>& magic)
            {
                if (Bytes.size() - Offset < magic.size())
                {
                    return false;
                }
                const bool match =
                    std::memcmp(Bytes.data() + Offset, magic.data(), magic.size()) == 0;
                Offset += magic.size();
                return match;
            }
        };

        // --- payload digest ---------------------------------------------------------------------

        // CRC-32 over the payload, chunked so a payload past 4 GB still hashes correctly (zlib's
        // length argument is 32-bit). This is an integrity check, not a security one: it exists so
        // a truncated or bit-rotted blob reads as a miss instead of being uploaded as texels.
        [[nodiscard]] u32 PayloadDigest(const std::span<const u8> payload)
        {
            constexpr usize Chunk = 1u << 20;
            uLong digest = ::crc32(0L, Z_NULL, 0);
            for (usize offset = 0; offset < payload.size(); offset += Chunk)
            {
                const usize length = std::min(Chunk, payload.size() - offset);
                digest = ::crc32(digest, payload.data() + offset, static_cast<uInt>(length));
            }
            return static_cast<u32>(digest);
        }

        // --- file helpers -----------------------------------------------------------------------

        [[nodiscard]] optional<vector<u8>> ReadFileBytes(const path& file)
        {
            std::ifstream stream(file, std::ios::binary | std::ios::ate);
            if (!stream)
            {
                return std::nullopt;
            }
            const std::streamsize size = stream.tellg();
            if (size < 0)
            {
                return std::nullopt;
            }
            stream.seekg(0, std::ios::beg);
            vector<u8> bytes(static_cast<usize>(size));
            if (size > 0 && !stream.read(reinterpret_cast<char*>(bytes.data()), size))
            {
                return std::nullopt;
            }
            return bytes;
        }

        // Writes to a sibling temp file and renames it into place. The write is not fsynced: this
        // is expendable content, and the payload digest already turns a half-written file into a
        // miss, so buying durability with a device flush per blob would cost the frame budget the
        // cache exists to protect and gain nothing a recomputation does not.
        [[nodiscard]] bool WriteFileAtomic(const path& file, const std::span<const u8> bytes)
        {
            path temp = file;
            temp += string(TempExtension);
            {
                std::ofstream stream(temp, std::ios::binary | std::ios::trunc);
                if (!stream)
                {
                    return false;
                }
                if (!bytes.empty() && !stream.write(reinterpret_cast<const char*>(bytes.data()),
                                                    static_cast<std::streamsize>(bytes.size())))
                {
                    return false;
                }
            }
            std::error_code ec;
            // std::filesystem::rename, not ::rename: the narrow path conversion a C rename needs is
            // lossy on Windows, which would fail every store under a non-ASCII user profile.
            std::filesystem::rename(temp, file, ec);
            if (ec)
            {
                std::filesystem::remove(temp, ec);
                return false;
            }
            return true;
        }

        /// @brief One indexed entry: where its blob lives, how big it is, and when it was last used.
        struct CacheEntry
        {
            /// @brief The id naming the entry's blob file.
            u64 FileId = 0;
            /// @brief The payload's byte length, as stored.
            u64 Bytes = 0;
            /// @brief The touch counter's value at the entry's last read or store.
            u64 Touch = 0;
        };
    }

    /// @brief The cache's whole mutable state, behind one mutex.
    struct DerivedDataCache::State
    {
        /// @brief The directory the cache owns its files within.
        path Root;
        /// @brief The generation every indexed entry was written under.
        string Generation;
        /// @brief The most entries the index may hold.
        u64 MaxEntries = 0;
        /// @brief The most payload bytes the index may account for.
        u64 MaxBytes = 0;

        /// @brief Guards every field below and the file I/O that maintains them.
        mutable std::mutex Mutex;
        /// @brief The indexed entries, keyed by the caller's key.
        std::unordered_map<string, CacheEntry> Entries;
        /// @brief Payload bytes summed across Entries.
        u64 TotalBytes = 0;
        /// @brief The id the next stored blob file takes.
        u64 NextFileId = 1;
        /// @brief Monotonic touch counter; the lowest value evicts first.
        u64 NextTouch = 1;
        /// @brief Reads that found a usable entry.
        u64 Hits = 0;
        /// @brief Reads that found nothing usable.
        u64 Misses = 0;
        /// @brief Entries written.
        u64 Stores = 0;
        /// @brief Entries dropped to stay within the caps.
        u64 Evictions = 0;

        [[nodiscard]] path IndexPath() const { return Root / string(IndexFileName); }

        [[nodiscard]] path BlobPath(const u64 fileId) const
        {
            return Root /
                   fmt::format("{}{:016x}{}", DerivedDataCache::FilePrefix, fileId, BlobExtension);
        }

        // Serializes the index and renames it into place. A failure leaves the prior index whole,
        // so the worst case is an entry on disk the index does not name — swept at the next Open.
        void WriteIndex()
        {
            vector<u8> bytes;
            bytes.insert(bytes.end(), IndexMagic.begin(), IndexMagic.end());
            Put<u32>(bytes, FormatVersion);
            Put<u32>(bytes, static_cast<u32>(Entries.size()));
            Put<u64>(bytes, NextFileId);
            Put<u64>(bytes, NextTouch);
            Put<u32>(bytes, static_cast<u32>(Generation.size()));
            PutBytes(bytes, Generation);
            for (const auto& [key, entry] : Entries)
            {
                Put<u32>(bytes, static_cast<u32>(key.size()));
                PutBytes(bytes, key);
                Put<u64>(bytes, entry.FileId);
                Put<u64>(bytes, entry.Bytes);
                Put<u64>(bytes, entry.Touch);
            }

            if (!WriteFileAtomic(IndexPath(), bytes))
            {
                Log::Warn("derived-data cache: cannot write the index at '{}'",
                          IndexPath().string());
            }
        }

        // Reads the index into the tables. Reports whether the file was readable *and* carried this
        // generation; anything else leaves the tables empty and has the caller wipe the root.
        [[nodiscard]] bool ReadIndex(const string& generation)
        {
            const optional<vector<u8>> bytes = ReadFileBytes(IndexPath());
            if (!bytes.has_value())
            {
                return false;
            }

            Reader reader{.Bytes = *bytes};
            u32 version = 0;
            u32 count = 0;
            u32 generationLength = 0;
            string storedGeneration;
            if (!reader.TakeMagic(IndexMagic) || !reader.Take(version) ||
                version != FormatVersion || !reader.Take(count) || count > MaxIndexEntries ||
                !reader.Take(NextFileId) || !reader.Take(NextTouch) ||
                !reader.Take(generationLength) || generationLength > MaxKeyLength ||
                !reader.TakeString(generationLength, storedGeneration))
            {
                return false;
            }
            if (storedGeneration != generation)
            {
                return false;
            }

            for (u32 i = 0; i < count; i++)
            {
                u32 keyLength = 0;
                string key;
                CacheEntry entry;
                if (!reader.Take(keyLength) || keyLength > MaxKeyLength ||
                    !reader.TakeString(keyLength, key) || !reader.Take(entry.FileId) ||
                    !reader.Take(entry.Bytes) || !reader.Take(entry.Touch))
                {
                    Entries.clear();
                    TotalBytes = 0;
                    return false;
                }
                Entries.emplace(std::move(key), entry);
            }
            return true;
        }

        // Deletes every file the cache owns within the root, index included, and empties the
        // tables. Scoped to the reserved prefix so a root shared with other content keeps it.
        void Wipe()
        {
            std::error_code ec;
            std::filesystem::remove(IndexPath(), ec);
            for (const std::filesystem::directory_entry& file :
                 std::filesystem::directory_iterator(Root, ec))
            {
                const string name = file.path().filename().string();
                if (name.starts_with(DerivedDataCache::FilePrefix))
                {
                    std::filesystem::remove(file.path(), ec);
                }
            }
            Entries.clear();
            TotalBytes = 0;
            NextFileId = 1;
            NextTouch = 1;
        }

        // Drops entries whose blob file is gone, removes stray temp files from an interrupted
        // write, and deletes blob files the index does not name.
        void Reconcile()
        {
            std::error_code ec;
            std::unordered_set<u64> indexed;
            for (const auto& [key, entry] : Entries)
            {
                indexed.insert(entry.FileId);
            }

            vector<string> lost;
            for (const auto& [key, entry] : Entries)
            {
                if (!std::filesystem::exists(BlobPath(entry.FileId), ec))
                {
                    lost.push_back(key);
                }
            }
            for (const string& key : lost)
            {
                TotalBytes -= Entries[key].Bytes;
                Entries.erase(key);
            }

            for (const std::filesystem::directory_entry& file :
                 std::filesystem::directory_iterator(Root, ec))
            {
                const string name = file.path().filename().string();
                if (!name.starts_with(DerivedDataCache::FilePrefix) || name == IndexFileName)
                {
                    continue;
                }
                if (name.ends_with(TempExtension))
                {
                    std::filesystem::remove(file.path(), ec);
                    continue;
                }
                if (!name.ends_with(BlobExtension))
                {
                    continue;
                }
                u64 fileId = 0;
                const string digits = name.substr(
                    DerivedDataCache::FilePrefix.size(),
                    name.size() - DerivedDataCache::FilePrefix.size() - BlobExtension.size());
                const auto parsed =
                    std::from_chars(digits.data(), digits.data() + digits.size(), fileId, 16);
                if (parsed.ec != std::errc{} || parsed.ptr != digits.data() + digits.size() ||
                    !indexed.contains(fileId))
                {
                    std::filesystem::remove(file.path(), ec);
                }
            }

            TotalBytes = 0;
            for (const auto& [key, entry] : Entries)
            {
                TotalBytes += entry.Bytes;
            }
        }

        // Evicts oldest-touched entries until both caps hold. Deletion goes through the index, so
        // the cache can only ever delete a file it wrote.
        void ApplyCaps()
        {
            while (Entries.size() > MaxEntries || TotalBytes > MaxBytes)
            {
                const auto oldest = std::ranges::min_element(Entries, {}, [](const auto& pair)
                                                             { return pair.second.Touch; });
                if (oldest == Entries.end())
                {
                    break;
                }
                std::error_code ec;
                std::filesystem::remove(BlobPath(oldest->second.FileId), ec);
                TotalBytes -= oldest->second.Bytes;
                Entries.erase(oldest);
                Evictions++;
            }
        }
    };

    DerivedDataCache::DerivedDataCache(Unique<State> state) : m_State(std::move(state)) {}

    DerivedDataCache::~DerivedDataCache() = default;

    Result<Unique<DerivedDataCache>> DerivedDataCache::Open(const DerivedDataCacheInfo& info)
    {
        std::error_code ec;
        std::filesystem::create_directories(info.Root, ec);
        if (!std::filesystem::is_directory(info.Root, ec))
        {
            return std::unexpected(
                fmt::format("cannot create the cache directory '{}'", info.Root.string()));
        }

        auto state = CreateUnique<State>();
        state->Root = info.Root;
        state->Generation = info.Generation;
        state->MaxEntries = info.MaxEntries;
        state->MaxBytes = info.MaxBytes;

        if (state->ReadIndex(info.Generation))
        {
            state->Reconcile();
        }
        else
        {
            // Either the cache holds another generation or its index is unreadable. Both mean every
            // entry is unusable, and re-deriving is exactly what a cold cache already costs.
            state->Wipe();
        }
        state->ApplyCaps();
        state->WriteIndex();

        return Unique<DerivedDataCache>(new DerivedDataCache(std::move(state)));
    }

    optional<vector<u8>> DerivedDataCache::Read(const string_view key)
    {
        const std::scoped_lock lock(m_State->Mutex);

        const auto it = m_State->Entries.find(string(key));
        if (it == m_State->Entries.end())
        {
            m_State->Misses++;
            return std::nullopt;
        }

        const path file = m_State->BlobPath(it->second.FileId);
        const optional<vector<u8>> bytes = ReadFileBytes(file);

        vector<u8> payload;
        bool usable = false;
        if (bytes.has_value())
        {
            Reader reader{.Bytes = *bytes};
            u32 version = 0;
            u32 digest = 0;
            u64 length = 0;
            if (reader.TakeMagic(BlobMagic) && reader.Take(version) && version == FormatVersion &&
                reader.Take(digest) && reader.Take(length) &&
                bytes->size() - reader.Offset == length)
            {
                payload.assign(bytes->data() + reader.Offset, bytes->data() + bytes->size());
                usable = PayloadDigest(payload) == digest;
            }
        }

        if (!usable)
        {
            std::error_code ec;
            std::filesystem::remove(file, ec);
            m_State->TotalBytes -= it->second.Bytes;
            m_State->Entries.erase(it);
            m_State->Misses++;
            m_State->WriteIndex();
            return std::nullopt;
        }

        it->second.Touch = m_State->NextTouch++;
        m_State->Hits++;
        m_State->WriteIndex();
        return payload;
    }

    bool DerivedDataCache::Store(const string_view key, const std::span<const u8> payload)
    {
        if (key.size() > MaxKeyLength)
        {
            Log::Warn("derived-data cache: a key of {} bytes exceeds the {}-byte limit", key.size(),
                      MaxKeyLength);
            return false;
        }

        const std::scoped_lock lock(m_State->Mutex);

        vector<u8> bytes;
        bytes.reserve(payload.size() + 24);
        bytes.insert(bytes.end(), BlobMagic.begin(), BlobMagic.end());
        Put<u32>(bytes, FormatVersion);
        Put<u32>(bytes, PayloadDigest(payload));
        Put<u64>(bytes, static_cast<u64>(payload.size()));
        bytes.insert(bytes.end(), payload.begin(), payload.end());

        const u64 fileId = m_State->NextFileId++;
        if (!WriteFileAtomic(m_State->BlobPath(fileId), bytes))
        {
            Log::Warn("derived-data cache: cannot write an entry under '{}'",
                      m_State->Root.string());
            return false;
        }

        const auto [it, inserted] = m_State->Entries.try_emplace(string(key));
        if (!inserted)
        {
            std::error_code ec;
            std::filesystem::remove(m_State->BlobPath(it->second.FileId), ec);
            m_State->TotalBytes -= it->second.Bytes;
        }
        it->second = CacheEntry{.FileId = fileId,
                                .Bytes = static_cast<u64>(payload.size()),
                                .Touch = m_State->NextTouch++};
        m_State->TotalBytes += it->second.Bytes;
        m_State->Stores++;

        m_State->ApplyCaps();
        m_State->WriteIndex();
        return true;
    }

    bool DerivedDataCache::Erase(const string_view key)
    {
        const std::scoped_lock lock(m_State->Mutex);

        const auto it = m_State->Entries.find(string(key));
        if (it == m_State->Entries.end())
        {
            return false;
        }
        std::error_code ec;
        std::filesystem::remove(m_State->BlobPath(it->second.FileId), ec);
        m_State->TotalBytes -= it->second.Bytes;
        m_State->Entries.erase(it);
        m_State->WriteIndex();
        return true;
    }

    void DerivedDataCache::Clear()
    {
        const std::scoped_lock lock(m_State->Mutex);
        m_State->Wipe();
        m_State->WriteIndex();
    }

    bool DerivedDataCache::Contains(const string_view key) const
    {
        const std::scoped_lock lock(m_State->Mutex);
        return m_State->Entries.contains(string(key));
    }

    DerivedDataCacheStats DerivedDataCache::GetStats() const
    {
        const std::scoped_lock lock(m_State->Mutex);
        return {
            .Entries = m_State->Entries.size(),
            .Bytes = m_State->TotalBytes,
            .Hits = m_State->Hits,
            .Misses = m_State->Misses,
            .Stores = m_State->Stores,
            .Evictions = m_State->Evictions,
        };
    }

    const path& DerivedDataCache::GetRoot() const
    {
        return m_State->Root;
    }

    const string& DerivedDataCache::GetGeneration() const
    {
        return m_State->Generation;
    }
}
