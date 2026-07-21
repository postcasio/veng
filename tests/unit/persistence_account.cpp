// Local-account-store tests: load-or-mint and its immediate durability, the consumer mint/validate
// hooks, the opaque profile's round trip, ephemeral mode, lock contention, and the surprise paths
// the irreplaceable-id posture defines — an unreadable record preserved before it is replaced, a
// future-format record refusing the open, and a failed mint-write surfacing as an error.

#include <doctest/doctest.h>

#include <Veng/Persistence/LocalAccountStore.h>

#include <support/TempPath.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>

using namespace Veng;

namespace
{
    // A fresh, unique account root per case under the process's scratch tree, removed on
    // destruction.
    struct TempRoot
    {
        path Dir;

        TempRoot()
        {
            static std::atomic<u64> counter{0};
            Dir = TestSupport::TempDir() /
                  fmt::format("account-{}", counter.fetch_add(1, std::memory_order_relaxed));
            std::filesystem::remove_all(Dir);
        }

        ~TempRoot() { std::filesystem::remove_all(Dir); }

        [[nodiscard]] path Record() const { return Dir / LocalAccountStore::FileName; }
        [[nodiscard]] path Preserved() const { return Dir / "account.corrupt"; }
    };

    // Appends a little-endian scalar, mirroring the store's own encoder — the surprise cases hand
    // the store records it did not write.
    template <typename T>
    void Put(vector<u8>& out, const T value)
    {
        const usize offset = out.size();
        out.resize(offset + sizeof(T));
        std::memcpy(out.data() + offset, &value, sizeof(T));
    }

    constexpr u64 AccountFileMagic = 0x315443412E474E56ULL;

    // A well-formed record at the given format version, so a case can vary exactly one field.
    vector<u8> MakeRecord(const u32 version, const Net::AccountId id)
    {
        vector<u8> bytes;
        Put(bytes, AccountFileMagic);
        Put(bytes, version);
        Put(bytes, id.Lo);
        Put(bytes, id.Hi);
        Put(bytes, static_cast<u64>(InvalidTypeId));
        Put(bytes, static_cast<u32>(0));
        return bytes;
    }

    void WriteBytes(const path& file, const vector<u8>& bytes)
    {
        std::filesystem::create_directories(file.parent_path());
        std::ofstream stream(file, std::ios::binary);
        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }

    vector<u8> ReadBytes(const path& file)
    {
        std::ifstream stream(file, std::ios::binary | std::ios::ate);
        const std::streamsize size = stream.tellg();
        stream.seekg(0, std::ios::beg);
        vector<u8> bytes(static_cast<usize>(size));
        if (size > 0)
        {
            stream.read(reinterpret_cast<char*>(bytes.data()), size);
        }
        return bytes;
    }
}

TEST_CASE("an absent record mints an account and persists it before the open returns")
{
    const TempRoot root;

    Result<LocalAccountStore> opened = LocalAccountStore::Open(root.Dir);
    REQUIRE(opened.has_value());
    CHECK(opened->GetId().IsValid());
    CHECK_FALSE(opened->IsEphemeral());
    CHECK_FALSE(opened->WasIdentityReset());
    // Durable before the id is handed out: the file exists while the store is still open, with no
    // profile write and no explicit save in between.
    CHECK(std::filesystem::is_regular_file(root.Record()));
}

TEST_CASE("a minted account id survives a reopen unchanged")
{
    const TempRoot root;

    Net::AccountId minted;
    {
        Result<LocalAccountStore> opened = LocalAccountStore::Open(root.Dir);
        REQUIRE(opened.has_value());
        minted = opened->GetId();
    }

    Result<LocalAccountStore> reopened = LocalAccountStore::Open(root.Dir);
    REQUIRE(reopened.has_value());
    CHECK(reopened->GetId() == minted);
    CHECK_FALSE(reopened->WasIdentityReset());
}

TEST_CASE("an unreadable record is preserved before a replacement is minted")
{
    const TempRoot root;
    const vector<u8> garbage{0x01, 0x02, 0x03, 0x04};
    WriteBytes(root.Record(), garbage);

    Result<LocalAccountStore> opened = LocalAccountStore::Open(root.Dir);
    REQUIRE(opened.has_value());
    CHECK(opened->GetId().IsValid());
    CHECK(opened->WasIdentityReset());
    // The bytes that were there outlive the replacement, verbatim.
    REQUIRE(std::filesystem::is_regular_file(root.Preserved()));
    CHECK(ReadBytes(root.Preserved()) == garbage);
    CHECK(ReadBytes(root.Record()) != garbage);
}

TEST_CASE("an id the consumer's validator rejects is preserved and re-minted")
{
    const TempRoot root;
    constexpr Net::AccountId stored{.Lo = 0x1111ULL, .Hi = 0x2222ULL};
    const vector<u8> record = MakeRecord(1, stored);
    WriteBytes(root.Record(), record);

    // A scheme the consumer owns: only ids with a zero high half belong to it.
    const LocalAccountInfo info{
        .MintId = [] { return Net::AccountId{.Lo = 0x99ULL, .Hi = 0ULL}; },
        .ValidateId = [](const Net::AccountId id) { return id.IsValid() && id.Hi == 0; },
    };

    Result<LocalAccountStore> opened = LocalAccountStore::Open(root.Dir, info);
    REQUIRE(opened.has_value());
    CHECK(opened->WasIdentityReset());
    CHECK(opened->GetId() == Net::AccountId{.Lo = 0x99ULL, .Hi = 0ULL});
    CHECK(ReadBytes(root.Preserved()) == record);
}

TEST_CASE("a record written by a newer format version refuses the open and is left alone")
{
    const TempRoot root;
    const vector<u8> record = MakeRecord(2, Net::AccountId{.Lo = 7ULL, .Hi = 0ULL});
    WriteBytes(root.Record(), record);

    const Result<LocalAccountStore> opened = LocalAccountStore::Open(root.Dir);
    REQUIRE_FALSE(opened.has_value());
    CHECK(opened.error().find("newer build") != string::npos);
    // Neither overwritten nor preserved-and-replaced: a downgrade must not cost the identity.
    CHECK(ReadBytes(root.Record()) == record);
    CHECK_FALSE(std::filesystem::exists(root.Preserved()));
}

TEST_CASE("a root the mint cannot be written to fails the open rather than returning a store")
{
    const TempRoot root;
    // A regular file where the root must be: nothing can be created under it, so the mint-write
    // has nowhere to land.
    WriteBytes(root.Dir, vector<u8>{0x00});

    const Result<LocalAccountStore> opened = LocalAccountStore::Open(root.Dir / "accounts");
    CHECK_FALSE(opened.has_value());
}

TEST_CASE("a consumer's MintId supplies the fresh id")
{
    const TempRoot root;
    const Net::AccountId scheme{.Lo = 0xABCDEF01ULL, .Hi = 0x40ULL};

    {
        Result<LocalAccountStore> opened = LocalAccountStore::Open(
            root.Dir, LocalAccountInfo{.MintId = [scheme] { return scheme; }});
        REQUIRE(opened.has_value());
        CHECK(opened->GetId() == scheme);
    }

    // The default mint is not consulted on the second open: the stored id is adopted as it stands.
    Result<LocalAccountStore> reopened = LocalAccountStore::Open(root.Dir);
    REQUIRE(reopened.has_value());
    CHECK(reopened->GetId() == scheme);
}

TEST_CASE("an ephemeral store writes nothing and holds no profile")
{
    const TempRoot root;

    LocalAccountStore account = LocalAccountStore::Ephemeral();
    CHECK(account.IsEphemeral());
    CHECK(account.GetId().IsValid());
    CHECK(account.GetProfile().IsEmpty());

    const VoidResult set = account.SetProfile(Net::Blob{.Type = 0x1234ULL, .Bytes = {1, 2, 3}});
    CHECK(set.has_value());
    CHECK(account.GetProfile().IsEmpty());
    // Nothing was resolved, created, or written — the root the case owns stays absent.
    CHECK_FALSE(std::filesystem::exists(root.Dir));
}

TEST_CASE("the profile round-trips opaquely across a reopen, leaving the id alone")
{
    const TempRoot root;
    const Net::Blob profile{.Type = 0x7E57000000000009ULL, .Bytes = {0xDE, 0xAD, 0x00, 0xBE, 0xEF}};

    Net::AccountId minted;
    {
        Result<LocalAccountStore> opened = LocalAccountStore::Open(root.Dir);
        REQUIRE(opened.has_value());
        minted = opened->GetId();
        REQUIRE(opened->GetProfile().IsEmpty());
        REQUIRE(opened->SetProfile(profile).has_value());
        CHECK(opened->GetProfile() == profile);
    }

    Result<LocalAccountStore> reopened = LocalAccountStore::Open(root.Dir);
    REQUIRE(reopened.has_value());
    CHECK(reopened->GetProfile() == profile);
    CHECK(reopened->GetId() == minted);
}

TEST_CASE("a second open of a locked account root fails loudly")
{
    const TempRoot root;

    const Result<LocalAccountStore> first = LocalAccountStore::Open(root.Dir);
    REQUIRE(first.has_value());

    const Result<LocalAccountStore> second = LocalAccountStore::Open(root.Dir);
    REQUIRE_FALSE(second.has_value());
    CHECK(second.error().find("locked") != string::npos);
}

TEST_CASE("a stray temporary beside an absent record is ignored and the mint completes")
{
    const TempRoot root;
    // What a crash mid-write leaves behind: the atomic write's temporary, with no record beside it.
    WriteBytes(root.Dir / "account.1a2b3c4d.tmp", vector<u8>{0xFF, 0xFF});

    Result<LocalAccountStore> opened = LocalAccountStore::Open(root.Dir);
    REQUIRE(opened.has_value());
    CHECK(opened->GetId().IsValid());
    CHECK_FALSE(opened->WasIdentityReset());
    CHECK(std::filesystem::is_regular_file(root.Record()));
}

TEST_CASE("a crashed profile write leaves the prior record byte-identical")
{
    const TempRoot root;
    const Net::Blob first{.Type = 0x11ULL, .Bytes = {1, 2, 3}};

    Net::AccountId minted;
    vector<u8> committed;
    {
        Result<LocalAccountStore> opened = LocalAccountStore::Open(root.Dir);
        REQUIRE(opened.has_value());
        minted = opened->GetId();
        REQUIRE(opened->SetProfile(first).has_value());
        committed = ReadBytes(root.Record());
    }

    // The replacement's bytes never reach the record: they die in the temporary the rename would
    // have promoted.
    WriteBytes(root.Dir / "account.99887766.tmp", vector<u8>{0xAA, 0xBB, 0xCC});
    CHECK(ReadBytes(root.Record()) == committed);

    Result<LocalAccountStore> reopened = LocalAccountStore::Open(root.Dir);
    REQUIRE(reopened.has_value());
    CHECK(reopened->GetId() == minted);
    CHECK(reopened->GetProfile() == first);
}
