// Durable-store tests: slot round-trips, the atomic whole-slot flush (the commit-record rename
// point), slot-lock contention, the per-family version migration at Read, the reflection walker's
// tolerant read across a component schema change, the rehydrate seam's elapsed-seconds plumbing
// (clamped >= 0), observer notification, and the untrusted-input hardening a public store API needs
// (stem validation and uniqueness, bounded counts, a scoped orphan sweep, a refused foreign slot).

#include <doctest/doctest.h>

#include <Veng/Persistence/Store.h>
#include <Veng/Reflection/Reflect.h>
#include <Veng/Reflection/Serialize.h>
#include <Veng/Reflection/TypeRegistry.h>
#include <Veng/Scene/Scene.h>

#include <support/TempPath.h>

#include <atomic>
#include <cstring>
#include <filesystem>
#include <fstream>
#include <span>

using namespace Veng;

namespace
{
    // A fresh, unique slot directory per case under the process's scratch tree, removed on
    // destruction.
    struct TempSlot
    {
        path Dir;

        TempSlot()
        {
            static std::atomic<u64> counter{0};
            Dir = TestSupport::TempDir() /
                  fmt::format("store-{}", counter.fetch_add(1, std::memory_order_relaxed));
            std::filesystem::remove_all(Dir);
        }

        ~TempSlot() { std::filesystem::remove_all(Dir); }
    };

    // The family under round-trip and migration exercise; the id is test-local.
    constexpr StoreFamilyId TestFamily{0x7E57000000000001ULL};
    constexpr StoreFamilyId OtherFamily{0x7E57000000000003ULL};

    StoreRecord MakeRecord(const i64 capturedAtWall, vector<u8> bytes)
    {
        StoreRecord record{.CapturedAtWall = capturedAtWall};
        record.Components.push_back(
            ComponentBlob{.Type = 0x7E57000000000002ULL, .Bytes = std::move(bytes)});
        return record;
    }

    // Appends a little-endian scalar, mirroring the store's own encoder — the hardening cases hand
    // the store files it did not write.
    template <typename T>
    void Put(vector<u8>& out, const T value)
    {
        const usize offset = out.size();
        out.resize(offset + sizeof(T));
        std::memcpy(out.data() + offset, &value, sizeof(T));
    }

    void WriteBytes(const path& file, const vector<u8>& bytes)
    {
        std::ofstream stream(file, std::ios::binary);
        stream.write(reinterpret_cast<const char*>(bytes.data()),
                     static_cast<std::streamsize>(bytes.size()));
    }

    constexpr u64 FamilyFileMagic = 0x315453562E474E56ULL;
    constexpr u64 CommitFileMagic = 0x31544D432E474E56ULL;

    // A commit record naming one family file: the shape the hardening cases perturb.
    vector<u8> MakeCommitRecord(const string& stem, const u64 generation)
    {
        vector<u8> commit;
        Put(commit, CommitFileMagic);
        Put(commit, generation);
        Put(commit, static_cast<u32>(1));
        Put(commit, TestFamily.Value);
        Put(commit, static_cast<u32>(stem.size()));
        commit.insert(commit.end(), stem.begin(), stem.end());
        Put(commit, generation);
        return commit;
    }
}

// The tolerant-read schema pair: V2 adds a field to V1's shape under matching field names.
namespace
{
    struct StoreProbeV1
    {
        u32 Count = 0;
    };

    struct StoreProbeV2
    {
        u32 Count = 0;
        u32 Extra = 77;
    };
}

VE_REFLECT(::StoreProbeV1, 0x7E57000000000010ULL)
VE_FIELD(Count)
VE_REFLECT_END();

VE_REFLECT(::StoreProbeV2, 0x7E57000000000011ULL)
VE_FIELD(Count)
VE_FIELD(Extra)
VE_REFLECT_END();

TEST_CASE("store round-trips records across close and reopen")
{
    const TempSlot slot;
    {
        Result<Unique<Store>> store = Store::Open(slot.Dir);
        REQUIRE(store);
        (*store)->RegisterFamily(StoreFamily{.Id = TestFamily, .FileStem = "test", .Version = 1});
        // A registered family nothing writes owns no file and must not break the reopen.
        (*store)->RegisterFamily(StoreFamily{
            .Id = StoreFamilyId{0x7E57000000000004ULL}, .FileStem = "unwritten", .Version = 1});
        (*store)->Write(TestFamily, StoreKey{.Lo = 1, .Hi = 2}, MakeRecord(1234, {9, 8, 7}));
        (*store)->Write(TestFamily, StoreKey{.Lo = 3, .Hi = 4}, MakeRecord(5678, {1}));
        CHECK((*store)->IsDirty());
        REQUIRE((*store)->Flush());
        CHECK(!(*store)->IsDirty());
        CHECK((*store)->GetGeneration() == 1);
        CHECK((*store)->GetSlotDirectory() == slot.Dir);
    }
    {
        Result<Unique<Store>> store = Store::Open(slot.Dir);
        REQUIRE(store);
        (*store)->RegisterFamily(StoreFamily{.Id = TestFamily, .FileStem = "test", .Version = 1});
        CHECK((*store)->GetGeneration() == 1);
        CHECK((*store)->GetRecordCount() == 2);
        const optional<StoreRecord> record = (*store)->Read(TestFamily, StoreKey{.Lo = 1, .Hi = 2});
        REQUIRE(record.has_value());
        CHECK(record->CapturedAtWall == 1234);
        REQUIRE(record->Components.size() == 1);
        CHECK(record->Components.front().Bytes == vector<u8>{9, 8, 7});
        CHECK(!(*store)->Read(TestFamily, StoreKey{.Lo = 9, .Hi = 9}).has_value());
    }
}

TEST_CASE("store erase removes a record durably, and EraseAll empties the slot")
{
    const TempSlot slot;
    {
        Result<Unique<Store>> store = Store::Open(slot.Dir);
        REQUIRE(store);
        (*store)->RegisterFamily(StoreFamily{.Id = TestFamily, .FileStem = "test", .Version = 1});
        (*store)->Write(TestFamily, StoreKey{.Lo = 1}, MakeRecord(1, {1}));
        (*store)->Write(TestFamily, StoreKey{.Lo = 2}, MakeRecord(2, {2}));
        REQUIRE((*store)->Flush());
        (*store)->Erase(TestFamily, StoreKey{.Lo = 1});
        REQUIRE((*store)->Flush());
    }
    {
        Result<Unique<Store>> store = Store::Open(slot.Dir);
        REQUIRE(store);
        CHECK((*store)->GetRecordCount() == 1);
        (*store)->EraseAll();
        REQUIRE((*store)->Flush());
    }
    {
        Result<Unique<Store>> store = Store::Open(slot.Dir);
        REQUIRE(store);
        CHECK((*store)->GetRecordCount() == 0);
    }
}

TEST_CASE("records of an unregistered family are preserved verbatim across a flush")
{
    const TempSlot slot;
    {
        Result<Unique<Store>> store = Store::Open(slot.Dir);
        REQUIRE(store);
        (*store)->RegisterFamily(StoreFamily{.Id = TestFamily, .FileStem = "test", .Version = 1});
        (*store)->RegisterFamily(StoreFamily{.Id = OtherFamily, .FileStem = "other", .Version = 1});
        (*store)->Write(TestFamily, StoreKey{.Lo = 1}, MakeRecord(1, {1}));
        (*store)->Write(OtherFamily, StoreKey{.Lo = 2}, MakeRecord(2, {2}));
        REQUIRE((*store)->Flush());
    }
    {
        // Only one family is registered this time; the other's records must survive the flush.
        Result<Unique<Store>> store = Store::Open(slot.Dir);
        REQUIRE(store);
        (*store)->RegisterFamily(StoreFamily{.Id = TestFamily, .FileStem = "test", .Version = 1});
        (*store)->Write(TestFamily, StoreKey{.Lo = 3}, MakeRecord(3, {3}));
        REQUIRE((*store)->Flush());
    }
    {
        Result<Unique<Store>> store = Store::Open(slot.Dir);
        REQUIRE(store);
        const optional<StoreRecord> preserved = (*store)->Read(OtherFamily, StoreKey{.Lo = 2});
        REQUIRE(preserved.has_value());
        CHECK(preserved->Components.front().Bytes == vector<u8>{2});
    }
}

TEST_CASE("a crash between family writes leaves the prior generation whole")
{
    const TempSlot slot;
    {
        Result<Unique<Store>> store = Store::Open(slot.Dir);
        REQUIRE(store);
        (*store)->RegisterFamily(StoreFamily{.Id = TestFamily, .FileStem = "test", .Version = 1});
        (*store)->RegisterFamily(StoreFamily{.Id = OtherFamily, .FileStem = "other", .Version = 1});
        (*store)->Write(TestFamily, StoreKey{.Lo = 1}, MakeRecord(1, {1, 1}));
        (*store)->Write(OtherFamily, StoreKey{.Lo = 2}, MakeRecord(2, {2, 2}));
        REQUIRE((*store)->Flush());
    }

    // Simulate a flush killed between its two family writes: one next-generation family file made
    // it to disk (with arbitrary partial content), the other did not, and the commit record was
    // never flipped. The committed generation must read back whole, and the orphans must sweep.
    {
        std::ofstream partial(slot.Dir / "test.2.vst", std::ios::binary);
        partial << "partial-write-garbage";
    }
    {
        std::ofstream tempCommit(slot.Dir / "slot.commit.2.tmp", std::ios::binary);
        tempCommit << "unrenamed-commit-record";
    }
    {
        Result<Unique<Store>> store = Store::Open(slot.Dir);
        REQUIRE(store);
        CHECK((*store)->GetGeneration() == 1);
        const optional<StoreRecord> test = (*store)->Read(TestFamily, StoreKey{.Lo = 1});
        const optional<StoreRecord> other = (*store)->Read(OtherFamily, StoreKey{.Lo = 2});
        REQUIRE(test.has_value());
        REQUIRE(other.has_value());
        CHECK(test->Components.front().Bytes == vector<u8>{1, 1});
        CHECK(other->Components.front().Bytes == vector<u8>{2, 2});
        CHECK(!std::filesystem::exists(slot.Dir / "test.2.vst"));
        CHECK(!std::filesystem::exists(slot.Dir / "slot.commit.2.tmp"));
    }
}

TEST_CASE("a superseded generation's files are replaced by the commit flip")
{
    const TempSlot slot;
    Result<Unique<Store>> store = Store::Open(slot.Dir);
    REQUIRE(store);
    (*store)->RegisterFamily(StoreFamily{.Id = TestFamily, .FileStem = "test", .Version = 1});
    (*store)->Write(TestFamily, StoreKey{.Lo = 1}, MakeRecord(1, {1}));
    REQUIRE((*store)->Flush());
    CHECK(std::filesystem::exists(slot.Dir / "test.1.vst"));
    (*store)->Write(TestFamily, StoreKey{.Lo = 1}, MakeRecord(2, {2}));
    REQUIRE((*store)->Flush());
    CHECK((*store)->GetGeneration() == 2);
    CHECK(std::filesystem::exists(slot.Dir / "test.2.vst"));
    CHECK(!std::filesystem::exists(slot.Dir / "test.1.vst"));
    CHECK(std::filesystem::exists(slot.Dir / "slot.commit"));
}

TEST_CASE("a second open of a locked slot fails loudly")
{
    const TempSlot slot;
    Result<Unique<Store>> first = Store::Open(slot.Dir);
    REQUIRE(first);
    const Result<Unique<Store>> second = Store::Open(slot.Dir);
    REQUIRE(!second);
    CHECK(second.error().find("locked") != string::npos);

    // Releasing the first store releases the lock; the slot opens again.
    first->reset();
    CHECK(Store::Open(slot.Dir));
}

TEST_CASE("a record written under one family version reads under the next through the migration")
{
    const TempSlot slot;
    {
        Result<Unique<Store>> store = Store::Open(slot.Dir);
        REQUIRE(store);
        (*store)->RegisterFamily(StoreFamily{.Id = TestFamily, .FileStem = "test", .Version = 1});
        (*store)->Write(TestFamily, StoreKey{.Lo = 5}, MakeRecord(50, {10, 20}));
        (*store)->Write(TestFamily, StoreKey{.Lo = 6}, MakeRecord(60, {30}));
        REQUIRE((*store)->Flush());
    }
    {
        // Reopen at version 2: the explicit migration appends a marker byte to every blob.
        Result<Unique<Store>> store = Store::Open(slot.Dir);
        REQUIRE(store);
        (*store)->RegisterFamily(StoreFamily{
            .Id = TestFamily,
            .FileStem = "test",
            .Version = 2,
            .Migrate = [](const u32 storedVersion, StoreRecord record) -> Result<StoreRecord>
            {
                CHECK(storedVersion == 1);
                for (ComponentBlob& blob : record.Components)
                {
                    blob.Bytes.push_back(0xAB);
                }
                return record;
            },
        });
        const optional<StoreRecord> migrated = (*store)->Read(TestFamily, StoreKey{.Lo = 5});
        REQUIRE(migrated.has_value());
        CHECK(migrated->Components.front().Bytes == vector<u8>{10, 20, 0xAB});
        // The migration dirties the family; the flush persists it under version 2.
        REQUIRE((*store)->Flush());
    }
    {
        // A version-2 reopen with no migration reads the lifted records directly — the version
        // lives in the family file's header, so the whole file is at 2 after the flush above.
        Result<Unique<Store>> store = Store::Open(slot.Dir);
        REQUIRE(store);
        (*store)->RegisterFamily(StoreFamily{.Id = TestFamily, .FileStem = "test", .Version = 2});
        const optional<StoreRecord> lifted = (*store)->Read(TestFamily, StoreKey{.Lo = 5});
        REQUIRE(lifted.has_value());
        CHECK(lifted->Components.front().Bytes == vector<u8>{10, 20, 0xAB});
        // The not-read-before-flush record was lifted by the flush's migrate-remaining sweep.
        const optional<StoreRecord> swept = (*store)->Read(TestFamily, StoreKey{.Lo = 6});
        REQUIRE(swept.has_value());
        CHECK(swept->Components.front().Bytes == vector<u8>{30, 0xAB});
    }
}

TEST_CASE("an older-version record with no migration reads as none")
{
    const TempSlot slot;
    {
        Result<Unique<Store>> store = Store::Open(slot.Dir);
        REQUIRE(store);
        (*store)->RegisterFamily(StoreFamily{.Id = TestFamily, .FileStem = "test", .Version = 1});
        (*store)->Write(TestFamily, StoreKey{.Lo = 5}, MakeRecord(50, {1}));
        REQUIRE((*store)->Flush());
    }
    {
        Result<Unique<Store>> store = Store::Open(slot.Dir);
        REQUIRE(store);
        (*store)->RegisterFamily(StoreFamily{.Id = TestFamily, .FileStem = "test", .Version = 2});
        CHECK(!(*store)->Read(TestFamily, StoreKey{.Lo = 5}).has_value());
    }
}

TEST_CASE("a component field added to the schema tolerant-reads an old record")
{
    TypeRegistry registry;
    registry.Register<StoreProbeV1>();
    registry.Register<StoreProbeV2>();

    // Encode the old shape, store it, and decode the stored bytes into the widened shape: the
    // name-keyed walker fills the shared field and leaves the added one at its default.
    const StoreProbeV1 old{.Count = 42};
    ComponentBlob blob{.Type = TypeIdOf<StoreProbeV1>()};
    WriteFields(blob.Bytes, &old, registry.Info(TypeIdOf<StoreProbeV1>()), registry);

    const TempSlot slot;
    Result<Unique<Store>> store = Store::Open(slot.Dir);
    REQUIRE(store);
    (*store)->RegisterFamily(StoreFamily{.Id = TestFamily, .FileStem = "test", .Version = 1});
    StoreRecord record{.CapturedAtWall = 1};
    record.Components.push_back(blob);
    (*store)->Write(TestFamily, StoreKey{.Lo = 1}, record);
    REQUIRE((*store)->Flush());

    const optional<StoreRecord> read = (*store)->Read(TestFamily, StoreKey{.Lo = 1});
    REQUIRE(read.has_value());
    StoreProbeV2 widened;
    REQUIRE(ReadFields(std::span(read->Components.front().Bytes), &widened,
                       registry.Info(TypeIdOf<StoreProbeV2>()), registry));
    CHECK(widened.Count == 42);
    CHECK(widened.Extra == 77);

    // The reverse direction: a widened record read back into the old shape skips the unknown field.
    const StoreProbeV2 wide{.Count = 9, .Extra = 5};
    ComponentBlob wideBlob{.Type = TypeIdOf<StoreProbeV2>()};
    WriteFields(wideBlob.Bytes, &wide, registry.Info(TypeIdOf<StoreProbeV2>()), registry);
    StoreProbeV1 narrowed;
    REQUIRE(ReadFields(std::span(wideBlob.Bytes), &narrowed,
                       registry.Info(TypeIdOf<StoreProbeV1>()), registry));
    CHECK(narrowed.Count == 9);
}

TEST_CASE("rehydrate receives elapsed wall seconds since capture, clamped >= 0")
{
    TypeRegistry registry;
    const Unique<Scene> scene = Scene::Create(registry);

    const TempSlot slot;
    Result<Unique<Store>> store = Store::Open(slot.Dir);
    REQUIRE(store);

    f64 observedElapsed = -1.0;
    StoreKey observedKey;
    (*store)->RegisterFamily(StoreFamily{
        .Id = TestFamily,
        .FileStem = "test",
        .Version = 1,
        .RehydrateKeys = [](Scene&) { return vector<StoreKey>{StoreKey{.Lo = 1}}; },
        .Rehydrate =
            [&](Scene&, const StoreKey key, const StoreRecord&, const f64 elapsedSeconds)
        {
            observedKey = key;
            observedElapsed = elapsedSeconds;
        },
    });

    // A record captured 100 seconds ago rehydrates with roughly that elapsed time.
    (*store)->Write(TestFamily, StoreKey{.Lo = 1},
                    MakeRecord(Store::WallClockSeconds() - 100, {1}));
    (*store)->RehydrateScene(*scene);
    CHECK(observedKey == StoreKey{.Lo = 1});
    CHECK(observedElapsed >= 99.0);
    CHECK(observedElapsed <= 200.0);

    // A future-dated capture (a regressed wall clock) clamps to zero, never negative.
    (*store)->Write(TestFamily, StoreKey{.Lo = 1},
                    MakeRecord(Store::WallClockSeconds() + 1000, {1}));
    (*store)->RehydrateScene(*scene);
    CHECK(observedElapsed == 0.0);
}

TEST_CASE("capture writes a family's records off a scene, stamped with the wall clock")
{
    TypeRegistry registry;
    const Unique<Scene> scene = Scene::Create(registry);

    const TempSlot slot;
    Result<Unique<Store>> store = Store::Open(slot.Dir);
    REQUIRE(store);
    (*store)->RegisterFamily(StoreFamily{
        .Id = TestFamily,
        .FileStem = "test",
        .Version = 1,
        .Capture = [](Scene&, vector<std::pair<StoreKey, StoreRecord>>& records)
        { records.emplace_back(StoreKey{.Lo = 7}, MakeRecord(0, {4, 5})); },
    });

    const i64 before = Store::WallClockSeconds();
    (*store)->CaptureScene(*scene);
    const optional<StoreRecord> captured = (*store)->Read(TestFamily, StoreKey{.Lo = 7});
    REQUIRE(captured.has_value());
    CHECK(captured->Components.front().Bytes == vector<u8>{4, 5});
    CHECK(captured->CapturedAtWall >= before);
}

TEST_CASE("observers fire per changed record, including from a nested write")
{
    const TempSlot slot;
    Result<Unique<Store>> store = Store::Open(slot.Dir);
    REQUIRE(store);
    (*store)->RegisterFamily(StoreFamily{.Id = TestFamily, .FileStem = "test", .Version = 1});

    vector<StoreKey> observed;
    bool nested = false;
    (*store)->Subscribe(
        [&](const StoreFamilyId family, const StoreKey key)
        {
            CHECK(family == TestFamily);
            observed.push_back(key);
            // A notified observer may write further records; the nested change notifies again.
            if (!nested && key.Lo == 1)
            {
                nested = true;
                (*store)->Write(TestFamily, StoreKey{.Lo = 2}, MakeRecord(2, {2}));
            }
        });

    (*store)->Write(TestFamily, StoreKey{.Lo = 1}, MakeRecord(1, {1}));
    REQUIRE(observed.size() == 2);
    CHECK(observed[0] == StoreKey{.Lo = 1});
    CHECK(observed[1] == StoreKey{.Lo = 2});

    observed.clear();
    (*store)->Erase(TestFamily, StoreKey{.Lo = 1});
    CHECK(observed == vector<StoreKey>{StoreKey{.Lo = 1}});

    // An erase of a record that is not there is not an effective mutation, so it notifies nobody.
    observed.clear();
    (*store)->Erase(TestFamily, StoreKey{.Lo = 99});
    CHECK(observed.empty());

    observed.clear();
    (*store)->EraseAll();
    CHECK(observed.size() == 1);
}

TEST_CASE("ForEachRecord visits a whole family, migrating as it goes")
{
    const TempSlot slot;
    {
        Result<Unique<Store>> store = Store::Open(slot.Dir);
        REQUIRE(store);
        (*store)->RegisterFamily(StoreFamily{.Id = TestFamily, .FileStem = "test", .Version = 1});
        (*store)->Write(TestFamily, StoreKey{.Lo = 1}, MakeRecord(1, {1}));
        (*store)->Write(TestFamily, StoreKey{.Lo = 2}, MakeRecord(2, {2}));
        REQUIRE((*store)->Flush());
    }
    {
        Result<Unique<Store>> store = Store::Open(slot.Dir);
        REQUIRE(store);
        (*store)->RegisterFamily(StoreFamily{
            .Id = TestFamily,
            .FileStem = "test",
            .Version = 2,
            .Migrate = [](u32, StoreRecord record) -> Result<StoreRecord>
            {
                record.Components.front().Bytes.push_back(0xCD);
                return record;
            },
        });
        usize visited = 0;
        (*store)->ForEachRecord(TestFamily,
                                [&](StoreKey, const StoreRecord& record)
                                {
                                    ++visited;
                                    CHECK(record.Components.front().Bytes.back() == 0xCD);
                                });
        CHECK(visited == 2);
    }
}

TEST_CASE("a file stem is validated before it is used as a path component")
{
    CHECK(Store::IsValidFileStem("test"));
    CHECK(Store::IsValidFileStem("veng.sessions"));
    CHECK(Store::IsValidFileStem("a-b_c.1"));
    CHECK(!Store::IsValidFileStem(""));
    CHECK(!Store::IsValidFileStem("."));
    CHECK(!Store::IsValidFileStem(".."));
    CHECK(!Store::IsValidFileStem("../../x"));
    CHECK(!Store::IsValidFileStem("a/b"));
    CHECK(!Store::IsValidFileStem("a b"));
    CHECK(!Store::IsValidFileStem(string(Store::MaxFileStemLength + 1, 'a')));
}

TEST_CASE("a commit record naming a traversing stem is refused at open")
{
    const TempSlot slot;
    std::filesystem::create_directories(slot.Dir);
    WriteBytes(slot.Dir / "slot.commit", MakeCommitRecord("../../x", 1));

    const Result<Unique<Store>> store = Store::Open(slot.Dir);
    REQUIRE(!store);
    CHECK(store.error().find("illegal file stem") != string::npos);
}

TEST_CASE("a commit record naming one stem twice is refused at open")
{
    const TempSlot slot;
    std::filesystem::create_directories(slot.Dir);
    vector<u8> commit;
    Put(commit, CommitFileMagic);
    Put(commit, static_cast<u64>(1));
    Put(commit, static_cast<u32>(2));
    for (const u64 id : {0x7E57000000000001ULL, 0x7E57000000000003ULL})
    {
        Put(commit, id);
        Put(commit, static_cast<u32>(4));
        const string stem = "same";
        commit.insert(commit.end(), stem.begin(), stem.end());
        Put(commit, static_cast<u64>(1));
    }
    WriteBytes(slot.Dir / "slot.commit", commit);
    // The first entry's file must load, so the duplicate is what the open reports.
    vector<u8> family;
    Put(family, FamilyFileMagic);
    Put(family, static_cast<u64>(0x7E57000000000001ULL));
    Put(family, static_cast<u32>(1));
    Put(family, static_cast<u32>(0));
    Put(family, static_cast<u64>(0));
    WriteBytes(slot.Dir / "same.1.vst", family);

    const Result<Unique<Store>> store = Store::Open(slot.Dir);
    REQUIRE(!store);
    CHECK(store.error().find("twice") != string::npos);
}

TEST_CASE("an implausible record count is refused rather than allocated")
{
    const TempSlot slot;
    std::filesystem::create_directories(slot.Dir);
    WriteBytes(slot.Dir / "slot.commit", MakeCommitRecord("test", 1));

    // A header claiming billions of records in a file holding none: the count is checked against
    // the bytes actually left before it drives a reserve.
    vector<u8> family;
    Put(family, FamilyFileMagic);
    Put(family, TestFamily.Value);
    Put(family, static_cast<u32>(1));
    Put(family, static_cast<u32>(0));
    Put(family, static_cast<u64>(0xFFFFFFFFFFULL));
    WriteBytes(slot.Dir / "test.1.vst", family);

    const Result<Unique<Store>> store = Store::Open(slot.Dir);
    REQUIRE(!store);
    CHECK(store.error().find("implausible record count") != string::npos);
}

TEST_CASE("an implausible component count is refused rather than allocated")
{
    const TempSlot slot;
    std::filesystem::create_directories(slot.Dir);
    WriteBytes(slot.Dir / "slot.commit", MakeCommitRecord("test", 1));

    vector<u8> family;
    Put(family, FamilyFileMagic);
    Put(family, TestFamily.Value);
    Put(family, static_cast<u32>(1));
    Put(family, static_cast<u32>(0));
    Put(family, static_cast<u64>(1));
    Put(family, static_cast<u64>(1));           // key low
    Put(family, static_cast<u64>(0));           // key high
    Put(family, static_cast<i64>(0));           // captured-at
    Put(family, static_cast<u32>(0xFFFFFFFFu)); // component count
    WriteBytes(slot.Dir / "test.1.vst", family);

    const Result<Unique<Store>> store = Store::Open(slot.Dir);
    REQUIRE(!store);
    CHECK(store.error().find("implausible component count") != string::npos);
}

TEST_CASE("opening a slot leaves files the store does not own alone")
{
    const TempSlot slot;
    std::filesystem::create_directories(slot.Dir);
    {
        std::ofstream foreign(slot.Dir / "notes.txt");
        foreign << "not the store's";
    }
    {
        std::ofstream foreign(slot.Dir / "photo.jpeg");
        foreign << "also not the store's";
    }

    const Result<Unique<Store>> store = Store::Open(slot.Dir);
    REQUIRE(store);
    CHECK(std::filesystem::exists(slot.Dir / "notes.txt"));
    CHECK(std::filesystem::exists(slot.Dir / "photo.jpeg"));
}

TEST_CASE("a slot holding an unrecognized control file is refused, not read as fresh")
{
    const TempSlot slot;
    std::filesystem::create_directories(slot.Dir);
    {
        // A control file under the store's reserved prefix that it does not recognize: another
        // format's commit point, or an older one of its own.
        std::ofstream foreign(slot.Dir / "slot.index", std::ios::binary);
        foreign << "some other format's commit point";
    }
    {
        std::ofstream data(slot.Dir / "records.1.vst", std::ios::binary);
        data << "data the sweep must not eat";
    }

    const Result<Unique<Store>> store = Store::Open(slot.Dir);
    REQUIRE(!store);
    CHECK(store.error().find("unrecognized control file") != string::npos);
    CHECK(std::filesystem::exists(slot.Dir / "records.1.vst"));
}

TEST_CASE("a commit record the store cannot read is refused, not read as fresh")
{
    const TempSlot slot;
    std::filesystem::create_directories(slot.Dir);
    {
        std::ofstream foreign(slot.Dir / "slot.commit", std::ios::binary);
        foreign << "a commit record under a magic this store does not know";
    }

    const Result<Unique<Store>> store = Store::Open(slot.Dir);
    REQUIRE(!store);
    CHECK(store.error().find("unreadable commit record") != string::npos);
}
