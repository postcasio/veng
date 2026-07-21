#include <Veng/Persistence/LocalAccountStore.h>

#include <Veng/Asset/AtomicFile.h>
#include <Veng/Log.h>

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

#include <cstring>
#include <filesystem>
#include <fstream>
#include <utility>

namespace Veng
{
    namespace
    {
        // The record's identity word, "VNG.ACT1" as little-endian bytes: a foreign or truncated
        // file is rejected before any field is trusted.
        constexpr u64 AccountFileMagic = 0x315443412E474E56ULL;

        // The format version this build writes and is willing to read. A record stamped higher was
        // written by a newer build and refuses the open — the user has downgraded, and overwriting
        // it would turn that into permanent identity loss.
        constexpr u32 AccountFormatVersion = 1;

        // The unreadable record's resting place and the advisory lock, both extending the record's
        // own reserved name.
        constexpr const char* CorruptFileSuffix = ".corrupt";
        constexpr const char* LockFileSuffix = ".lock";

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

        // Why a stored record was not adopted, which decides what happens to it: a future-version
        // record is left alone and fails the open, an unreadable one is preserved and replaced.
        enum class RecordFault
        {
            None,
            Unreadable,
            FutureVersion,
        };

        struct ParsedRecord
        {
            Net::AccountId Id;
            Net::Blob Profile;
            RecordFault Fault = RecordFault::None;
        };

        // Decodes the record's bytes. The profile's byte count is checked against the bytes
        // actually remaining before it drives an allocation — the file may not be one this build
        // wrote.
        [[nodiscard]] ParsedRecord ParseRecord(const vector<u8>& bytes)
        {
            ParsedRecord parsed;
            usize cursor = 0;
            u64 magic = 0;
            u32 version = 0;
            if (!Take(bytes, cursor, magic) || magic != AccountFileMagic ||
                !Take(bytes, cursor, version))
            {
                parsed.Fault = RecordFault::Unreadable;
                return parsed;
            }
            if (version > AccountFormatVersion)
            {
                parsed.Fault = RecordFault::FutureVersion;
                return parsed;
            }

            u32 profileBytes = 0;
            if (!Take(bytes, cursor, parsed.Id.Lo) || !Take(bytes, cursor, parsed.Id.Hi) ||
                !Take(bytes, cursor, parsed.Profile.Type) || !Take(bytes, cursor, profileBytes) ||
                profileBytes > bytes.size() - cursor)
            {
                parsed.Fault = RecordFault::Unreadable;
                return parsed;
            }

            parsed.Profile.Bytes.assign(bytes.begin() + static_cast<isize>(cursor),
                                        bytes.begin() + static_cast<isize>(cursor + profileBytes));
            return parsed;
        }

        [[nodiscard]] vector<u8> EncodeRecord(const Net::AccountId id, const Net::Blob& profile)
        {
            vector<u8> bytes;
            Put(bytes, AccountFileMagic);
            Put(bytes, AccountFormatVersion);
            Put(bytes, id.Lo);
            Put(bytes, id.Hi);
            Put(bytes, profile.Type);
            Put(bytes, static_cast<u32>(profile.Bytes.size()));
            bytes.insert(bytes.end(), profile.Bytes.begin(), profile.Bytes.end());
            return bytes;
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
    }

    /// @brief The store's held state: the record, the root it lives at, and the advisory lock.
    struct LocalAccountStore::State
    {
        /// @brief The directory the record lives in; empty when the store is ephemeral.
        path Root;
        /// @brief The account id this store presents.
        Net::AccountId Id;
        /// @brief The consumer-defined profile, held verbatim.
        Net::Blob Profile;
        /// @brief Whether Open replaced an unreadable or rejected record.
        bool IdentityReset = false;
#if defined(_WIN32)
        /// @brief The held exclusive account lock's file handle (opened with no sharing).
        HANDLE LockHandle = INVALID_HANDLE_VALUE;
#else
        /// @brief The held exclusive account lock's file descriptor.
        int LockFd = -1;
#endif

        ~State()
        {
#if defined(_WIN32)
            if (LockHandle != INVALID_HANDLE_VALUE)
            {
                ::CloseHandle(LockHandle);
            }
#else
            if (LockFd >= 0)
            {
                ::close(LockFd);
            }
#endif
        }
    };

    LocalAccountStore::LocalAccountStore(Unique<State> state) : m_State(std::move(state)) {}

    LocalAccountStore::~LocalAccountStore() = default;

    LocalAccountStore::LocalAccountStore(LocalAccountStore&&) noexcept = default;

    LocalAccountStore& LocalAccountStore::operator=(LocalAccountStore&&) noexcept = default;

    Result<LocalAccountStore> LocalAccountStore::Open(const path& root, LocalAccountInfo info)
    {
        std::error_code ec;
        std::filesystem::create_directories(root, ec);
        if (ec)
        {
            return std::unexpected(fmt::format("cannot create account root '{}'", root.string()));
        }

        auto state = Unique<State>(new State{});
        state->Root = root;

        // The exclusive account lock: held for the store's lifetime and released by the OS on any
        // process exit, so a crash leaves no stale lock. Two unlocked opens on an empty root both
        // mint, and the loser has already published its id, so contention fails loudly here.
        const path lockFile = root / (string{FileName} + LockFileSuffix);
#if defined(_WIN32)
        // Opening with no share mode is the exclusive lock: a second open fails with a sharing
        // violation, and the handle is released by the OS on any process exit.
        state->LockHandle = ::CreateFileW(lockFile.wstring().c_str(), GENERIC_READ | GENERIC_WRITE,
                                          0, nullptr, OPEN_ALWAYS, FILE_ATTRIBUTE_NORMAL, nullptr);
        if (state->LockHandle == INVALID_HANDLE_VALUE)
        {
            if (::GetLastError() == ERROR_SHARING_VIOLATION)
            {
                return std::unexpected(
                    fmt::format("the account at '{}' is locked by another process", root.string()));
            }
            return std::unexpected(fmt::format("cannot open account lock '{}'", lockFile.string()));
        }
#else
        state->LockFd = ::open(lockFile.string().c_str(), O_CREAT | O_RDWR, 0644);
        if (state->LockFd < 0)
        {
            return std::unexpected(fmt::format("cannot open account lock '{}'", lockFile.string()));
        }
        if (::flock(state->LockFd, LOCK_EX | LOCK_NB) != 0)
        {
            ::close(state->LockFd);
            state->LockFd = -1;
            return std::unexpected(
                fmt::format("the account at '{}' is locked by another process", root.string()));
        }
#endif

        const path recordFile = root / FileName;
        const bool present = std::filesystem::is_regular_file(recordFile, ec);
        if (present)
        {
            const Result<vector<u8>> bytes = ReadFileBytes(recordFile);
            ParsedRecord parsed;
            if (bytes)
            {
                parsed = ParseRecord(*bytes);
            }
            else
            {
                parsed.Fault = RecordFault::Unreadable;
            }

            if (parsed.Fault == RecordFault::FutureVersion)
            {
                return std::unexpected(fmt::format(
                    "the account record at '{}' was written by a newer build of this application",
                    recordFile.string()));
            }

            const bool accepted =
                parsed.Fault == RecordFault::None &&
                (info.ValidateId ? info.ValidateId(parsed.Id) : parsed.Id.IsValid());
            if (accepted)
            {
                state->Id = parsed.Id;
                state->Profile = std::move(parsed.Profile);
                return LocalAccountStore(std::move(state));
            }

            // The id is irreplaceable, so the bytes that were there outlive the replacement. A
            // preserve that cannot be done is a failed open rather than a mint over the evidence.
            const path preservedFile = root / (string{FileName} + CorruptFileSuffix);
            std::filesystem::rename(recordFile, preservedFile, ec);
            if (ec)
            {
                return std::unexpected(
                    fmt::format("the account record at '{}' is unreadable and cannot be preserved "
                                "at '{}': {}",
                                recordFile.string(), preservedFile.string(), ec.message()));
            }
            Log::Warn("account: the record at {} is unreadable or outside this application's id "
                      "scheme; it is preserved at {} and a fresh account is minted",
                      recordFile.string(), preservedFile.string());
            state->IdentityReset = true;
        }

        state->Id = info.MintId ? info.MintId() : Net::GenerateAccountId();

        // Written before the id is handed out: a caller that has already keyed records on an id
        // the next launch will not present is the one failure this class exists to prevent.
        const vector<u8> encoded = EncodeRecord(state->Id, state->Profile);
        if (const VoidResult written = WriteFileAtomic(recordFile, encoded); !written)
        {
            return std::unexpected(written.error());
        }
        return LocalAccountStore(std::move(state));
    }

    LocalAccountStore LocalAccountStore::Ephemeral(LocalAccountInfo info)
    {
        auto state = Unique<State>(new State{});
        state->Id = info.MintId ? info.MintId() : Net::GenerateAccountId();
        return LocalAccountStore(std::move(state));
    }

    Net::AccountId LocalAccountStore::GetId() const
    {
        return m_State->Id;
    }

    bool LocalAccountStore::IsEphemeral() const
    {
        return m_State->Root.empty();
    }

    bool LocalAccountStore::WasIdentityReset() const
    {
        return m_State->IdentityReset;
    }

    const Net::Blob& LocalAccountStore::GetProfile() const
    {
        return m_State->Profile;
    }

    VoidResult LocalAccountStore::SetProfile(Net::Blob profile)
    {
        if (IsEphemeral())
        {
            // An ephemeral store owns no record to update: the profile exists to be persisted, and
            // there is nothing here to persist it to.
            return {};
        }

        const vector<u8> encoded = EncodeRecord(m_State->Id, profile);
        if (const VoidResult written = WriteFileAtomic(m_State->Root / FileName, encoded); !written)
        {
            // The durable profile is the one GetProfile reports, so a failed write leaves the
            // in-memory copy alone rather than diverging from disk.
            return written;
        }
        m_State->Profile = std::move(profile);
        return {};
    }
}
