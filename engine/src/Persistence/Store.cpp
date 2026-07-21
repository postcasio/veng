#include <Veng/Persistence/Store.h>

#include <Veng/Assert.h>
#include <Veng/Log.h>
#include <Veng/Scene/Scene.h>

#include <fmt/format.h>

#if defined(_WIN32)
#define WIN32_LEAN_AND_MEAN
#define NOMINMAX
#include <windows.h>
#else
#include <fcntl.h>
#include <sys/file.h>
#include <unistd.h>
#endif

#include <algorithm>
#include <chrono>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <unordered_map>
#include <unordered_set>

namespace Veng
{
    namespace
    {
        // The on-disk identity words, "VNG.VST1" and "VNG.CMT1" as little-endian bytes. A family
        // file and the commit record each open with a magic word, so a foreign or truncated file is
        // rejected before any field is trusted.
        constexpr u64 FamilyFileMagic = 0x315453562E474E56ULL;
        constexpr u64 CommitFileMagic = 0x31544D432E474E56ULL;

        // The slot's fixed file names: the committed generation record (the atomic-flush commit
        // point) and the exclusive slot lock.
        constexpr const char* CommitFileName = "slot.commit";
        constexpr const char* LockFileName = "slot.lock";

        // The store owns every file name starting with this prefix; an unrecognized one means the
        // directory is not a slot this store wrote, and the open is refused rather than proceeding
        // as if the slot were fresh.
        constexpr string_view ReservedFilePrefix = "slot.";

        // The smallest byte cost of one record's fixed header (key pair, wall stamp, component
        // count) and of one component's header (type id, byte count). Counts read from a file are
        // checked against the bytes actually remaining before they drive an allocation.
        constexpr usize MinimumRecordBytes = 8 + 8 + 8 + 4;
        constexpr usize MinimumComponentBytes = 8 + 4;
        // One commit-record entry: family id, stem length, at least an empty stem, file generation.
        constexpr usize MinimumCommitEntryBytes = 8 + 4 + 8;

        // Appends a little-endian scalar to the output buffer.
        template <typename T>
        void Put(vector<u8>& out, const T value)
        {
            const usize offset = out.size();
            out.resize(offset + sizeof(T));
            std::memcpy(out.data() + offset, &value, sizeof(T));
        }

        // Reads a little-endian scalar at the cursor, advancing it; false past the end.
        template <typename T>
        [[nodiscard]] bool Take(const vector<u8>& in, usize& cursor, T& value)
        {
            if (cursor + sizeof(T) > in.size())
            {
                return false;
            }
            std::memcpy(&value, in.data() + cursor, sizeof(T));
            cursor += sizeof(T);
            return true;
        }

        // Whether every character of a name is a decimal digit, over a non-empty range.
        [[nodiscard]] bool IsDigits(const string_view text)
        {
            return !text.empty() &&
                   std::ranges::all_of(text, [](const char c) { return c >= '0' && c <= '9'; });
        }

        // Whether a file name is one of the store's own family files, `<stem>.<generation>.vst`.
        [[nodiscard]] bool IsFamilyFileName(const string_view name)
        {
            constexpr string_view extension = ".vst";
            if (!name.ends_with(extension))
            {
                return false;
            }
            const string_view head = name.substr(0, name.size() - extension.size());
            const usize dot = head.rfind('.');
            if (dot == string_view::npos)
            {
                return false;
            }
            return IsDigits(head.substr(dot + 1)) && Store::IsValidFileStem(head.substr(0, dot));
        }

        // Whether a file name is a commit record written but not yet renamed into place.
        [[nodiscard]] bool IsCommitTempFileName(const string_view name)
        {
            constexpr string_view extension = ".tmp";
            const usize headLength = string_view(CommitFileName).size() + 1;
            if (!name.ends_with(extension) || name.size() <= headLength + extension.size() ||
                name.substr(0, headLength) != fmt::format("{}.", CommitFileName))
            {
                return false;
            }
            return IsDigits(name.substr(headLength, name.size() - headLength - extension.size()));
        }

        // Whether a file name belongs to the store: its control files, its family files, and the
        // commit record's temporaries.
        [[nodiscard]] bool IsStoreFileName(const string_view name)
        {
            return name == CommitFileName || name == LockFileName || IsCommitTempFileName(name) ||
                   IsFamilyFileName(name);
        }

        // Reads a whole file into a byte buffer. Recoverable: a missing/unreadable file is an error.
        [[nodiscard]] Result<vector<u8>> ReadFileBytes(const path& file)
        {
            std::ifstream stream(file, std::ios::binary | std::ios::ate);
            if (!stream)
            {
                return std::unexpected(fmt::format("cannot open '{}'", file.string()));
            }
            const std::streamsize size = stream.tellg();
            stream.seekg(0, std::ios::beg);
            vector<u8> bytes(static_cast<usize>(size));
            if (size > 0 && !stream.read(reinterpret_cast<char*>(bytes.data()), size))
            {
                return std::unexpected(fmt::format("cannot read '{}'", file.string()));
            }
            return bytes;
        }

        // Writes a byte buffer to a file and syncs it to the device, so a later commit never
        // references a family file the OS has not yet made durable.
        [[nodiscard]] VoidResult WriteFileSynced(const path& file, const vector<u8>& bytes)
        {
#if defined(_WIN32)
            const HANDLE handle = ::CreateFileW(file.wstring().c_str(), GENERIC_WRITE, 0, nullptr,
                                                CREATE_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
            if (handle == INVALID_HANDLE_VALUE)
            {
                return std::unexpected(fmt::format("cannot open '{}' for writing", file.string()));
            }
            usize written = 0;
            while (written < bytes.size())
            {
                const usize remaining = bytes.size() - written;
                const DWORD chunk =
                    remaining > 0x7FFFFFFFu ? 0x7FFFFFFFu : static_cast<DWORD>(remaining);
                DWORD n = 0;
                if (::WriteFile(handle, bytes.data() + written, chunk, &n, nullptr) == FALSE)
                {
                    ::CloseHandle(handle);
                    return std::unexpected(fmt::format("write to '{}' failed", file.string()));
                }
                written += n;
            }
            // FlushFileBuffers is the fsync equivalent: it forces the written bytes to the device.
            if (::FlushFileBuffers(handle) == FALSE)
            {
                ::CloseHandle(handle);
                return std::unexpected(fmt::format("fsync of '{}' failed", file.string()));
            }
            ::CloseHandle(handle);
            return {};
#else
            const int fd = ::open(file.string().c_str(), O_CREAT | O_WRONLY | O_TRUNC, 0644);
            if (fd < 0)
            {
                return std::unexpected(fmt::format("cannot open '{}' for writing", file.string()));
            }
            usize written = 0;
            while (written < bytes.size())
            {
                const ssize_t n = ::write(fd, bytes.data() + written, bytes.size() - written);
                if (n < 0)
                {
                    ::close(fd);
                    return std::unexpected(fmt::format("write to '{}' failed", file.string()));
                }
                written += static_cast<usize>(n);
            }
            if (::fsync(fd) != 0)
            {
                ::close(fd);
                return std::unexpected(fmt::format("fsync of '{}' failed", file.string()));
            }
            ::close(fd);
            return {};
#endif
        }

        // Syncs a directory so a rename inside it is durable (the commit flip's second half).
        void SyncDirectory(const path& directory)
        {
#if defined(_WIN32)
            // Windows has no directory-handle fsync; a file's FlushFileBuffers already commits its
            // directory entry, so the commit rename's durability rides the synced write above.
            (void)directory;
#else
            const int fd = ::open(directory.string().c_str(), O_RDONLY);
            if (fd >= 0)
            {
                ::fsync(fd);
                ::close(fd);
            }
#endif
        }

        // Hashes a StoreKey for the in-memory record tables.
        struct StoreKeyHash
        {
            [[nodiscard]] usize operator()(const StoreKey& key) const noexcept
            {
                // SplitMix64-style fold of the two halves.
                u64 h = key.Lo + 0x9E3779B97F4A7C15ULL;
                h = (h ^ (h >> 30)) * 0xBF58476D1CE4E5B9ULL;
                h ^= key.Hi + (h >> 27);
                h = (h ^ (h >> 31)) * 0x94D049BB133111EBULL;
                return static_cast<usize>(h ^ (h >> 33));
            }
        };

        // Hashes a StoreFamilyId for the family table.
        struct StoreFamilyIdHash
        {
            [[nodiscard]] usize operator()(const StoreFamilyId& id) const noexcept
            {
                return std::hash<u64>{}(id.Value);
            }
        };

        /// @brief One family's live state: its records, file identity, versions, and hooks.
        struct FamilyState
        {
            /// @brief The registered hooks and current version; Id invalid until registered.
            StoreFamily Info;
            /// @brief The family file's name stem (from the commit record, or the registration).
            string FileStem;
            /// @brief The version the not-yet-migrated in-memory records were stored under.
            u32 StoredVersion = 1;
            /// @brief The generation suffix of the family's committed on-disk file; 0 for none.
            u64 FileGeneration = 0;
            /// @brief Whether the family has unflushed writes.
            bool Dirty = false;
            /// @brief Whether a family was registered this process (hooks and Version are live).
            bool Registered = false;
            /// @brief Whether the missing-migration condition was already logged (log once).
            bool MigrationGapLogged = false;
            /// @brief The record table.
            std::unordered_map<StoreKey, StoreRecord, StoreKeyHash> Records;
            /// @brief Keys already migrated (or written fresh) at the registered version, while
            ///        StoredVersion still lags it.
            std::unordered_set<StoreKey, StoreKeyHash> Migrated;
        };
    }

    struct Store::State
    {
        /// @brief The slot directory every file lives in.
        path SlotDir;
#if defined(_WIN32)
        /// @brief The held exclusive slot lock's file handle (opened with no sharing).
        HANDLE LockHandle = INVALID_HANDLE_VALUE;
#else
        /// @brief The held exclusive slot lock's file descriptor.
        int LockFd = -1;
#endif
        /// @brief The committed generation (the commit record's); 0 for a fresh slot.
        u64 Generation = 0;
        /// @brief Every family with records or a registration, keyed by id.
        std::unordered_map<StoreFamilyId, FamilyState, StoreFamilyIdHash> Families;
        /// @brief The record-change observers, each fired per changed key (Write/Erase/EraseAll).
        vector<function<void(StoreFamilyId, StoreKey)>> Observers;

        /// @brief Notifies every observer of one changed record.
        void NotifyChanged(const StoreFamilyId family, const StoreKey key)
        {
            for (const function<void(StoreFamilyId, StoreKey)>& observer : Observers)
            {
                observer(family, key);
            }
        }
    };

    namespace
    {
        // Serializes one family's table into its file image (header + records).
        [[nodiscard]] vector<u8> EncodeFamilyFile(const StoreFamilyId id, const FamilyState& family,
                                                  const u32 version)
        {
            vector<u8> out;
            Put(out, FamilyFileMagic);
            Put(out, id.Value);
            Put(out, version);
            Put(out, static_cast<u32>(0));
            Put(out, static_cast<u64>(family.Records.size()));
            for (const auto& [key, record] : family.Records)
            {
                Put(out, key.Lo);
                Put(out, key.Hi);
                Put(out, record.CapturedAtWall);
                Put(out, static_cast<u32>(record.Components.size()));
                for (const ComponentBlob& component : record.Components)
                {
                    Put(out, component.Type);
                    Put(out, static_cast<u32>(component.Bytes.size()));
                    out.insert(out.end(), component.Bytes.begin(), component.Bytes.end());
                }
            }
            return out;
        }

        // Parses a family file image into a family state (records + stored version). Recoverable:
        // a malformed file is an error and the slot open fails rather than silently dropping data.
        [[nodiscard]] VoidResult DecodeFamilyFile(const vector<u8>& in,
                                                  const StoreFamilyId expected, FamilyState& family)
        {
            usize cursor = 0;
            u64 magic = 0;
            u64 id = 0;
            u32 version = 0;
            u32 reserved = 0;
            u64 count = 0;
            if (!Take(in, cursor, magic) || magic != FamilyFileMagic)
            {
                return std::unexpected(string("bad family file magic"));
            }
            if (!Take(in, cursor, id) || id != expected.Value)
            {
                return std::unexpected(string("family file id does not match the commit record"));
            }
            if (!Take(in, cursor, version) || !Take(in, cursor, reserved) ||
                !Take(in, cursor, count))
            {
                return std::unexpected(string("truncated family file header"));
            }
            // The count comes off disk and drives an allocation, so it is checked against the bytes
            // actually left in the file before it is trusted.
            if (count > (in.size() - cursor) / MinimumRecordBytes)
            {
                return std::unexpected(string("implausible record count"));
            }
            family.StoredVersion = version;
            family.Records.reserve(static_cast<usize>(count));
            for (u64 i = 0; i < count; ++i)
            {
                StoreKey key;
                StoreRecord record;
                u32 componentCount = 0;
                if (!Take(in, cursor, key.Lo) || !Take(in, cursor, key.Hi) ||
                    !Take(in, cursor, record.CapturedAtWall) || !Take(in, cursor, componentCount))
                {
                    return std::unexpected(string("truncated family record"));
                }
                if (componentCount > (in.size() - cursor) / MinimumComponentBytes)
                {
                    return std::unexpected(string("implausible component count"));
                }
                record.Components.resize(componentCount);
                for (ComponentBlob& component : record.Components)
                {
                    u32 byteCount = 0;
                    if (!Take(in, cursor, component.Type) || !Take(in, cursor, byteCount))
                    {
                        return std::unexpected(string("truncated component header"));
                    }
                    if (cursor + byteCount > in.size())
                    {
                        return std::unexpected(string("truncated component bytes"));
                    }
                    component.Bytes.assign(in.begin() + static_cast<isize>(cursor),
                                           in.begin() + static_cast<isize>(cursor + byteCount));
                    cursor += byteCount;
                }
                family.Records.emplace(key, std::move(record));
            }
            return {};
        }

        // Runs a family's migration for one record read from an older version. Returns nullopt —
        // and logs, once per family for a missing hook — when the record cannot reach the
        // registered version.
        [[nodiscard]] optional<StoreRecord> MigrateRecord(FamilyState& family, const StoreKey key,
                                                          const StoreRecord& record)
        {
            if (!family.Info.Migrate)
            {
                if (!family.MigrationGapLogged)
                {
                    Log::Warn("store: family '{}' file is version {} but version {} is "
                              "registered with no migration; its old records read as none",
                              family.FileStem, family.StoredVersion, family.Info.Version);
                    family.MigrationGapLogged = true;
                }
                return std::nullopt;
            }
            Result<StoreRecord> migrated = family.Info.Migrate(family.StoredVersion, record);
            if (!migrated)
            {
                Log::Warn("store: family '{}' migration of a version-{} record failed: {}",
                          family.FileStem, family.StoredVersion, migrated.error());
                return std::nullopt;
            }
            family.Records[key] = *migrated;
            family.Migrated.insert(key);
            family.Dirty = true;
            return std::move(*migrated);
        }

        // Migrates every not-yet-migrated record so the family file can be written under the
        // registered version; a record that cannot migrate is dropped from the table (logged).
        void MigrateRemaining(FamilyState& family)
        {
            vector<StoreKey> pending;
            for (const auto& [key, record] : family.Records)
            {
                if (!family.Migrated.contains(key))
                {
                    pending.push_back(key);
                }
            }
            for (const StoreKey key : pending)
            {
                if (!MigrateRecord(family, key, family.Records.at(key)).has_value())
                {
                    family.Records.erase(key);
                    family.Dirty = true;
                }
            }
            family.StoredVersion = family.Info.Version;
            family.Migrated.clear();
        }
    }

    Store::Store(Unique<State> state) : m_State(std::move(state)) {}

    Store::~Store()
    {
#if defined(_WIN32)
        if (m_State && m_State->LockHandle != INVALID_HANDLE_VALUE)
        {
            ::CloseHandle(m_State->LockHandle);
        }
#else
        if (m_State && m_State->LockFd >= 0)
        {
            ::close(m_State->LockFd);
        }
#endif
    }

    bool Store::IsValidFileStem(const string_view stem)
    {
        if (stem.empty() || stem.size() > MaxFileStemLength || stem == "." || stem == "..")
        {
            return false;
        }
        return std::ranges::all_of(stem,
                                   [](const char c)
                                   {
                                       return (c >= 'A' && c <= 'Z') || (c >= 'a' && c <= 'z') ||
                                              (c >= '0' && c <= '9') || c == '.' || c == '_' ||
                                              c == '-';
                                   });
    }

    Result<Unique<Store>> Store::Open(const path& slotDirectory)
    {
        std::error_code ec;
        std::filesystem::create_directories(slotDirectory, ec);
        if (ec)
        {
            return std::unexpected(
                fmt::format("cannot create slot directory '{}'", slotDirectory.string()));
        }

        auto state = Unique<State>(new State{});
        state->SlotDir = slotDirectory;

        // The exclusive slot lock: held for the store's lifetime and released by the OS on any
        // process exit, so a crash leaves no stale lock. Contention fails loudly here.
        const path lockFile = slotDirectory / LockFileName;
#if defined(_WIN32)
        // Opening with no share mode is the exclusive lock: a second process's open fails with a
        // sharing violation, and the handle is released by the OS on any process exit.
        state->LockHandle = ::CreateFileW(lockFile.wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
                                          0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (state->LockHandle == INVALID_HANDLE_VALUE)
        {
            if (::GetLastError() == ERROR_SHARING_VIOLATION)
            {
                return std::unexpected(
                    fmt::format("slot '{}' is locked by another process", slotDirectory.string()));
            }
            return std::unexpected(fmt::format("cannot open slot lock '{}'", lockFile.string()));
        }
#else
        state->LockFd = ::open(lockFile.string().c_str(), O_CREAT | O_RDWR, 0644);
        if (state->LockFd < 0)
        {
            return std::unexpected(fmt::format("cannot open slot lock '{}'", lockFile.string()));
        }
        if (::flock(state->LockFd, LOCK_EX | LOCK_NB) != 0)
        {
            ::close(state->LockFd);
            state->LockFd = -1;
            return std::unexpected(
                fmt::format("slot '{}' is locked by another process", slotDirectory.string()));
        }
#endif

        // The directory listing drives both the reserved-prefix check below and the orphan sweep at
        // the end, so it is taken once, up front.
        vector<string> presentFiles;
        for (const auto& entry : std::filesystem::directory_iterator(slotDirectory, ec))
        {
            if (entry.is_regular_file(ec))
            {
                presentFiles.push_back(entry.path().filename().string());
            }
        }

        // A file under the store's reserved prefix that the store does not recognize means this
        // directory is not a slot this store wrote. Reporting it is the whole point: reading such a
        // directory as a fresh empty slot would present someone else's data as an empty store and
        // then sweep it.
        for (const string& name : presentFiles)
        {
            if (name.starts_with(ReservedFilePrefix) && !IsStoreFileName(name))
            {
                return std::unexpected(
                    fmt::format("slot '{}' holds an unrecognized control file '{}'",
                                slotDirectory.string(), name));
            }
        }

        // An absent commit record is a fresh slot (an empty store); a present one names the
        // committed generation and every family file of that generation.
        const path commitFile = slotDirectory / CommitFileName;
        if (std::filesystem::exists(commitFile, ec))
        {
            Result<vector<u8>> bytes = ReadFileBytes(commitFile);
            if (!bytes)
            {
                return std::unexpected(bytes.error());
            }
            usize cursor = 0;
            u64 magic = 0;
            u32 familyCount = 0;
            if (!Take(*bytes, cursor, magic) || magic != CommitFileMagic ||
                !Take(*bytes, cursor, state->Generation) || !Take(*bytes, cursor, familyCount))
            {
                return std::unexpected(
                    fmt::format("unreadable commit record '{}'", commitFile.string()));
            }
            if (familyCount > (bytes->size() - cursor) / MinimumCommitEntryBytes)
            {
                return std::unexpected(fmt::format("implausible family count in commit record '{}'",
                                                   commitFile.string()));
            }
            std::unordered_set<string> claimedStems;
            for (u32 i = 0; i < familyCount; ++i)
            {
                StoreFamilyId id;
                u32 stemLength = 0;
                if (!Take(*bytes, cursor, id.Value) || !Take(*bytes, cursor, stemLength) ||
                    cursor + stemLength > bytes->size())
                {
                    return std::unexpected(
                        fmt::format("truncated commit record '{}'", commitFile.string()));
                }
                string stem(reinterpret_cast<const char*>(bytes->data() + cursor), stemLength);
                cursor += stemLength;
                u64 fileGeneration = 0;
                if (!Take(*bytes, cursor, fileGeneration))
                {
                    return std::unexpected(
                        fmt::format("truncated commit record '{}'", commitFile.string()));
                }
                // The stem is interpolated into a path on both load and write, so it is validated
                // before it is used rather than trusted because it came off disk.
                if (!IsValidFileStem(stem))
                {
                    return std::unexpected(fmt::format("commit record '{}' names an illegal file "
                                                       "stem '{}'",
                                                       commitFile.string(), stem));
                }
                if (!claimedStems.insert(stem).second)
                {
                    return std::unexpected(
                        fmt::format("commit record '{}' names the file stem '{}' twice",
                                    commitFile.string(), stem));
                }

                FamilyState family;
                family.FileStem = std::move(stem);
                family.FileGeneration = fileGeneration;
                const path familyFile =
                    slotDirectory / fmt::format("{}.{}.vst", family.FileStem, fileGeneration);
                Result<vector<u8>> fileBytes = ReadFileBytes(familyFile);
                if (!fileBytes)
                {
                    return std::unexpected(fileBytes.error());
                }
                if (const VoidResult decoded = DecodeFamilyFile(*fileBytes, id, family); !decoded)
                {
                    return std::unexpected(
                        fmt::format("family file '{}': {}", familyFile.string(), decoded.error()));
                }
                state->Families.emplace(id, std::move(family));
            }
        }

        // Sweep the store's own files the commit record does not reference — a crashed flush's
        // orphaned next-generation files. The committed generation is whole by construction, so
        // anything of ours beyond it is garbage; anything that is not ours is left alone, since
        // opening a slot must never mean emptying a directory.
        std::unordered_set<string> referenced{CommitFileName, LockFileName};
        for (const auto& [id, family] : state->Families)
        {
            referenced.insert(fmt::format("{}.{}.vst", family.FileStem, family.FileGeneration));
        }
        for (const string& name : presentFiles)
        {
            if (!referenced.contains(name) &&
                (IsFamilyFileName(name) || IsCommitTempFileName(name)))
            {
                std::filesystem::remove(slotDirectory / name, ec);
            }
        }

        return Unique<Store>(new Store(std::move(state)));
    }

    void Store::RegisterFamily(StoreFamily family)
    {
        VE_ASSERT(family.Id.IsValid(), "store: a registered family needs a valid id");
        VE_ASSERT(IsValidFileStem(family.FileStem), "store: '{}' is not a legal family file stem",
                  family.FileStem);
        for (const auto& [id, existing] : m_State->Families)
        {
            VE_ASSERT(id == family.Id || !existing.Registered ||
                          existing.FileStem != family.FileStem,
                      "store: file stem '{}' is claimed by two families", family.FileStem);
        }

        FamilyState& state = m_State->Families[family.Id];
        VE_ASSERT(!state.Registered, "store: family '{}' registered twice", family.FileStem);
        if (state.FileStem.empty())
        {
            // No on-disk file yet: the table starts empty at the registered version.
            state.FileStem = family.FileStem;
            state.StoredVersion = family.Version;
        }
        state.Registered = true;
        state.Info = std::move(family);
    }

    optional<StoreRecord> Store::Read(const StoreFamilyId family, const StoreKey key)
    {
        const auto familyIt = m_State->Families.find(family);
        if (familyIt == m_State->Families.end())
        {
            return std::nullopt;
        }
        FamilyState& state = familyIt->second;
        const auto recordIt = state.Records.find(key);
        if (recordIt == state.Records.end())
        {
            return std::nullopt;
        }
        // A record stored under an older family version migrates here, at Read: the explicit
        // per-family migration function lifts it to the registered version (and the table keeps
        // the lifted record, persisted on the next flush).
        if (state.Registered && state.StoredVersion != state.Info.Version &&
            !state.Migrated.contains(key))
        {
            return MigrateRecord(state, key, recordIt->second);
        }
        return recordIt->second;
    }

    void Store::Write(const StoreFamilyId family, const StoreKey key, StoreRecord record)
    {
        VE_ASSERT(family.IsValid(), "store: Write needs a valid family id");
        FamilyState& state = m_State->Families[family];
        state.Records[key] = std::move(record);
        state.Dirty = true;
        if (state.Registered && state.StoredVersion != state.Info.Version)
        {
            // A fresh write is current-version data; it needs no migration at a later Read.
            state.Migrated.insert(key);
        }
        m_State->NotifyChanged(family, key);
    }

    void Store::Subscribe(function<void(StoreFamilyId, StoreKey)> onChanged)
    {
        m_State->Observers.push_back(std::move(onChanged));
    }

    void Store::ForEachRecord(const StoreFamilyId family,
                              const function<void(StoreKey, const StoreRecord&)>& visit)
    {
        const auto familyIt = m_State->Families.find(family);
        if (familyIt == m_State->Families.end())
        {
            return;
        }
        FamilyState& state = familyIt->second;
        // Lift the whole table to the registered version first, so the visit never hands out a
        // stale-version record (and never migrates — which writes the table — mid-iteration).
        if (state.Registered && state.StoredVersion != state.Info.Version)
        {
            MigrateRemaining(state);
        }
        for (const auto& [key, record] : state.Records)
        {
            visit(key, record);
        }
    }

    void Store::Erase(const StoreFamilyId family, const StoreKey key)
    {
        const auto familyIt = m_State->Families.find(family);
        if (familyIt == m_State->Families.end())
        {
            return;
        }
        if (familyIt->second.Records.erase(key) > 0)
        {
            familyIt->second.Dirty = true;
            familyIt->second.Migrated.erase(key);
            m_State->NotifyChanged(family, key);
        }
    }

    void Store::EraseAll()
    {
        for (auto& [id, family] : m_State->Families)
        {
            if (!family.Records.empty())
            {
                vector<StoreKey> erased;
                erased.reserve(family.Records.size());
                for (const auto& [key, record] : family.Records)
                {
                    erased.push_back(key);
                }
                family.Records.clear();
                family.Migrated.clear();
                family.Dirty = true;
                for (const StoreKey key : erased)
                {
                    m_State->NotifyChanged(id, key);
                }
            }
        }
    }

    VoidResult Store::Flush()
    {
        State& state = *m_State;
        const bool anyDirty = std::ranges::any_of(state.Families, [](const auto& entry)
                                                  { return entry.second.Dirty; });
        if (!anyDirty)
        {
            return {};
        }

        // Dirty families write under the next generation's suffixed names first; the committed
        // record still references only the prior generation's files, so a crash below leaves the
        // prior generation fully readable — never a mixed-generation slot.
        const u64 nextGeneration = state.Generation + 1;
        vector<path> superseded;
        for (auto& [id, family] : state.Families)
        {
            if (!family.Dirty)
            {
                continue;
            }
            // A version-lagged registered family lifts every remaining record before the write,
            // so the file carries one version — its header's — for all records.
            if (family.Registered && family.StoredVersion != family.Info.Version)
            {
                MigrateRemaining(family);
            }
            const u32 version = family.Registered ? family.Info.Version : family.StoredVersion;
            const path file =
                state.SlotDir / fmt::format("{}.{}.vst", family.FileStem, nextGeneration);
            if (const VoidResult written =
                    WriteFileSynced(file, EncodeFamilyFile(id, family, version));
                !written)
            {
                return written;
            }
        }

        // The commit record: every family's committed file (dirty ones at the new generation, clean
        // ones keeping their old file), written to a temp and renamed into place — the rename is
        // the whole slot's commit point. A registered family that has never been written owns no
        // file yet and stays out of the record.
        vector<u8> commit;
        Put(commit, CommitFileMagic);
        Put(commit, nextGeneration);
        const u32 fileCount = static_cast<u32>(std::ranges::count_if(
            state.Families, [](const auto& entry)
            { return entry.second.Dirty || entry.second.FileGeneration != 0; }));
        Put(commit, fileCount);
        for (const auto& [id, family] : state.Families)
        {
            if (!family.Dirty && family.FileGeneration == 0)
            {
                continue;
            }
            Put(commit, id.Value);
            Put(commit, static_cast<u32>(family.FileStem.size()));
            commit.insert(commit.end(), family.FileStem.begin(), family.FileStem.end());
            Put(commit, family.Dirty ? nextGeneration : family.FileGeneration);
        }
        const path commitTemp =
            state.SlotDir / fmt::format("{}.{}.tmp", CommitFileName, nextGeneration);
        const path commitFile = state.SlotDir / CommitFileName;
        if (const VoidResult written = WriteFileSynced(commitTemp, commit); !written)
        {
            return written;
        }
        // std::filesystem::rename, not ::rename: the narrow conversion a C rename needs is lossy
        // for a slot directory under a non-ASCII path on Windows, which would fail every flush
        // while the rest of the write path worked.
        std::error_code ec;
        std::filesystem::rename(commitTemp, commitFile, ec);
        if (ec)
        {
            return std::unexpected(
                fmt::format("cannot commit slot record '{}'", commitFile.string()));
        }
        SyncDirectory(state.SlotDir);

        // Committed: advance the generations and drop each replaced family file (best effort —
        // the open-time sweep also collects any straggler).
        for (auto& [id, family] : state.Families)
        {
            if (!family.Dirty)
            {
                continue;
            }
            if (family.FileGeneration != 0)
            {
                superseded.push_back(state.SlotDir / fmt::format("{}.{}.vst", family.FileStem,
                                                                 family.FileGeneration));
            }
            family.FileGeneration = nextGeneration;
            family.Dirty = false;
        }
        state.Generation = nextGeneration;
        for (const path& file : superseded)
        {
            std::filesystem::remove(file, ec);
        }
        return {};
    }

    void Store::CaptureScene(Scene& scene)
    {
        const i64 now = WallClockSeconds();
        for (auto& [id, family] : m_State->Families)
        {
            if (!family.Registered || !family.Info.Capture)
            {
                continue;
            }
            vector<std::pair<StoreKey, StoreRecord>> records;
            family.Info.Capture(scene, records);
            for (auto& [key, record] : records)
            {
                record.CapturedAtWall = now;
                Write(id, key, std::move(record));
            }
        }
    }

    void Store::RehydrateScene(Scene& scene)
    {
        const i64 now = WallClockSeconds();
        for (auto& [id, family] : m_State->Families)
        {
            if (!family.Registered || !family.Info.RehydrateKeys || !family.Info.Rehydrate)
            {
                continue;
            }
            for (const StoreKey key : family.Info.RehydrateKeys(scene))
            {
                const optional<StoreRecord> record = Read(id, key);
                if (!record.has_value())
                {
                    continue;
                }
                // Clamped >= 0 at this seam: wall clocks regress (NTP, suspend, manual change),
                // and a rehydrate must never see negative elapsed time.
                const f64 elapsed = std::max(0.0, static_cast<f64>(now - record->CapturedAtWall));
                family.Info.Rehydrate(scene, key, *record, elapsed);
            }
        }
    }

    bool Store::IsDirty() const
    {
        return std::ranges::any_of(m_State->Families,
                                   [](const auto& entry) { return entry.second.Dirty; });
    }

    const path& Store::GetSlotDirectory() const
    {
        return m_State->SlotDir;
    }

    u64 Store::GetGeneration() const
    {
        return m_State->Generation;
    }

    usize Store::GetRecordCount() const
    {
        usize count = 0;
        for (const auto& [id, family] : m_State->Families)
        {
            count += family.Records.size();
        }
        return count;
    }

    i64 Store::WallClockSeconds()
    {
        return std::chrono::duration_cast<std::chrono::seconds>(
                   std::chrono::system_clock::now().time_since_epoch())
            .count();
    }
}
