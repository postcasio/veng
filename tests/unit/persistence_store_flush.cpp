// The store's background flush: a snapshot on the calling thread, the file write on a job. The
// properties pinned: a background flush commits exactly the files a waited one does; a write made
// while a flush is in flight is neither lost nor folded into the in-flight write; flushes requested
// during one write coalesce into one more; a failed write leaves the prior generation readable and
// its families dirty until a retry lands; a family's flush interval holds it out of flushes (its
// committed file kept) until the interval passes, and a full flush ignores it; and closing a store
// commits a write it has not started yet. Writes are staged through the store's own RunWrite seam,
// so nothing here sleeps or races.

#include <doctest/doctest.h>

#include <Veng/Persistence/Store.h>
#include <Veng/Task/TaskSystem.h>

#include <support/TempPath.h>

#include <atomic>
#include <filesystem>
#include <fstream>
#include <iterator>
#include <map>

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
                  fmt::format("store-flush-{}", counter.fetch_add(1, std::memory_order_relaxed));
            std::filesystem::remove_all(Dir);
        }

        ~TempSlot() { std::filesystem::remove_all(Dir); }
    };

    constexpr StoreFamilyId FastFamily{0x7E57030000000001ULL};
    constexpr StoreFamilyId SlowFamily{0x7E57030000000002ULL};

    StoreRecord MakeRecord(const u8 value)
    {
        StoreRecord record{.CapturedAtWall = value};
        record.Components.push_back(ComponentBlob{.Type = 0x7E57030000000010ULL, .Bytes = {value}});
        return record;
    }

    // The single byte a record written by MakeRecord carries; nullopt when the record is absent.
    optional<u8> ValueOf(Store& store, const StoreFamilyId family, const u64 key)
    {
        const optional<StoreRecord> record = store.Read(family, StoreKey{.Lo = key});
        if (!record.has_value())
        {
            return std::nullopt;
        }
        return record->Components.front().Bytes.front();
    }

    void RegisterFamilies(Store& store, const f64 slowInterval = 0.0)
    {
        store.RegisterFamily(StoreFamily{.Id = FastFamily, .FileStem = "fast"});
        store.RegisterFamily(StoreFamily{
            .Id = SlowFamily, .FileStem = "slow", .FlushIntervalSeconds = slowInterval});
    }

    // A RunWrite seam that holds every job until the test runs it.
    struct HeldWrites
    {
        vector<function<void()>> Jobs;

        [[nodiscard]] StoreInfo Info()
        {
            return StoreInfo{.RunWrite = [this](function<void()> job)
                             { Jobs.push_back(std::move(job)); }};
        }

        void RunAll()
        {
            vector<function<void()>> jobs;
            jobs.swap(Jobs);
            for (const function<void()>& job : jobs)
            {
                job();
            }
        }
    };

    // Every file in a slot but its lock, by name, with its bytes.
    std::map<string, vector<char>> SlotFiles(const path& dir)
    {
        std::map<string, vector<char>> files;
        for (const auto& entry : std::filesystem::directory_iterator(dir))
        {
            const string name = entry.path().filename().string();
            if (name == "slot.lock" || !entry.is_regular_file())
            {
                continue;
            }
            std::ifstream stream(entry.path(), std::ios::binary);
            files.emplace(name, vector<char>(std::istreambuf_iterator<char>(stream), {}));
        }
        return files;
    }

    // Copies a slot's committed files to another directory, so the state on disk can be opened
    // while the original store still holds its slot's lock.
    void CopySlot(const path& from, const path& to)
    {
        std::filesystem::create_directories(to);
        for (const auto& entry : std::filesystem::directory_iterator(from))
        {
            if (entry.is_regular_file() && entry.path().filename() != "slot.lock")
            {
                std::filesystem::copy_file(entry.path(), to / entry.path().filename());
            }
        }
    }
}

TEST_CASE("a background flush commits the files a waited flush does, and reopens to the same "
          "records")
{
    const TempSlot background;
    const TempSlot waited;
    for (const TempSlot* slot : {&background, &waited})
    {
        Result<Unique<Store>> store = Store::Open(slot->Dir);
        REQUIRE(store);
        RegisterFamilies(**store);
        (*store)->Write(FastFamily, StoreKey{.Lo = 1}, MakeRecord(11));
        (*store)->Write(FastFamily, StoreKey{.Lo = 2}, MakeRecord(12));
        (*store)->Write(SlowFamily, StoreKey{.Lo = 3}, MakeRecord(13));
        if (slot == &background)
        {
            // The default executor: a thread of the store's own, which the close waits for.
            (*store)->Flush();
        }
        else
        {
            REQUIRE((*store)->FlushAndWait());
        }
    }

    CHECK(SlotFiles(background.Dir) == SlotFiles(waited.Dir));
    Result<Unique<Store>> reopened = Store::Open(background.Dir);
    REQUIRE(reopened);
    CHECK((*reopened)->GetGeneration() == 1);
    CHECK(ValueOf(**reopened, FastFamily, 1) == 11);
    CHECK(ValueOf(**reopened, FastFamily, 2) == 12);
    CHECK(ValueOf(**reopened, SlowFamily, 3) == 13);
}

TEST_CASE("a write made while a flush is in flight lands in the next flush, not the in-flight one")
{
    const TempSlot slot;
    const TempSlot copy;
    HeldWrites held;
    {
        Result<Unique<Store>> store = Store::Open(slot.Dir, held.Info());
        REQUIRE(store);
        RegisterFamilies(**store);
        (*store)->Write(FastFamily, StoreKey{.Lo = 1}, MakeRecord(1));
        (*store)->Flush();
        REQUIRE(held.Jobs.size() == 1);
        CHECK((*store)->IsDirty());
        CHECK((*store)->GetGeneration() == 0);

        // Written after the snapshot: the in-flight write must still commit the old value whole.
        (*store)->Write(FastFamily, StoreKey{.Lo = 1}, MakeRecord(2));
        (*store)->Write(FastFamily, StoreKey{.Lo = 2}, MakeRecord(3));
        CHECK(ValueOf(**store, FastFamily, 1) == 2);
        held.RunAll();
        CHECK((*store)->GetGeneration() == 1);
        CHECK((*store)->IsDirty());
        CopySlot(slot.Dir, copy.Dir);

        (*store)->Flush();
        held.RunAll();
        CHECK((*store)->GetGeneration() == 2);
        CHECK(!(*store)->IsDirty());
    }

    Result<Unique<Store>> committedFirst = Store::Open(copy.Dir);
    REQUIRE(committedFirst);
    CHECK(ValueOf(**committedFirst, FastFamily, 1) == 1);
    CHECK(!ValueOf(**committedFirst, FastFamily, 2).has_value());

    Result<Unique<Store>> reopened = Store::Open(slot.Dir);
    REQUIRE(reopened);
    CHECK(ValueOf(**reopened, FastFamily, 1) == 2);
    CHECK(ValueOf(**reopened, FastFamily, 2) == 3);
}

TEST_CASE("flushes requested during one in-flight write coalesce into one more write")
{
    const TempSlot slot;
    HeldWrites held;
    {
        Result<Unique<Store>> store = Store::Open(slot.Dir, held.Info());
        REQUIRE(store);
        RegisterFamilies(**store);
        (*store)->Write(FastFamily, StoreKey{.Lo = 1}, MakeRecord(1));
        (*store)->Flush();
        (*store)->Write(FastFamily, StoreKey{.Lo = 2}, MakeRecord(2));
        (*store)->Flush();
        (*store)->Write(SlowFamily, StoreKey{.Lo = 3}, MakeRecord(3));
        (*store)->Write(FastFamily, StoreKey{.Lo = 1}, MakeRecord(4));
        (*store)->Flush();

        // One job carries the in-flight write and the one queued behind it.
        CHECK(held.Jobs.size() == 1);
        held.RunAll();
        CHECK((*store)->GetGeneration() == 2);
        CHECK(!(*store)->IsDirty());
    }

    Result<Unique<Store>> reopened = Store::Open(slot.Dir);
    REQUIRE(reopened);
    CHECK((*reopened)->GetGeneration() == 2);
    CHECK(ValueOf(**reopened, FastFamily, 1) == 4);
    CHECK(ValueOf(**reopened, FastFamily, 2) == 2);
    CHECK(ValueOf(**reopened, SlowFamily, 3) == 3);
}

TEST_CASE("a failed write leaves the prior generation readable and its families dirty")
{
    const TempSlot slot;
    const TempSlot copy;
    HeldWrites held;
    {
        Result<Unique<Store>> store = Store::Open(slot.Dir, held.Info());
        REQUIRE(store);
        RegisterFamilies(**store);
        (*store)->Write(FastFamily, StoreKey{.Lo = 1}, MakeRecord(1));
        (*store)->Write(SlowFamily, StoreKey{.Lo = 2}, MakeRecord(2));
        REQUIRE((*store)->FlushAndWait());

        // A directory squatting on one family's next-generation file name fails that write
        // whichever family the write reaches first.
        const path blocker = slot.Dir / "slow.2.vst";
        std::filesystem::create_directory(blocker);
        (*store)->Write(FastFamily, StoreKey{.Lo = 1}, MakeRecord(3));
        (*store)->Write(SlowFamily, StoreKey{.Lo = 2}, MakeRecord(4));
        CHECK(!(*store)->FlushAndWait());
        CHECK((*store)->GetGeneration() == 1);
        CHECK((*store)->IsDirty());

        // The background path fails the same way and keeps the families dirty for a retry.
        (*store)->Flush();
        held.RunAll();
        CHECK((*store)->GetGeneration() == 1);
        CHECK((*store)->IsDirty());
        CopySlot(slot.Dir, copy.Dir);

        std::filesystem::remove(blocker);
        (*store)->Flush();
        held.RunAll();
        CHECK((*store)->GetGeneration() == 2);
        CHECK(!(*store)->IsDirty());
    }

    Result<Unique<Store>> prior = Store::Open(copy.Dir);
    REQUIRE(prior);
    CHECK((*prior)->GetGeneration() == 1);
    CHECK(ValueOf(**prior, FastFamily, 1) == 1);
    CHECK(ValueOf(**prior, SlowFamily, 2) == 2);

    Result<Unique<Store>> reopened = Store::Open(slot.Dir);
    REQUIRE(reopened);
    CHECK(ValueOf(**reopened, FastFamily, 1) == 3);
    CHECK(ValueOf(**reopened, SlowFamily, 2) == 4);
}

TEST_CASE("a family with a flush interval is held out until it passes, and a full flush takes it")
{
    const TempSlot slot;
    const TempSlot copy;
    f64 now = 0.0;
    {
        Result<Unique<Store>> store = Store::Open(
            slot.Dir, StoreInfo{.Clock = [&now] { return now; },
                                .RunWrite = [](const function<void()>& job) { job(); }});
        REQUIRE(store);
        RegisterFamilies(**store, 10.0);
        (*store)->Write(FastFamily, StoreKey{.Lo = 1}, MakeRecord(1));
        (*store)->Write(SlowFamily, StoreKey{.Lo = 2}, MakeRecord(2));
        REQUIRE((*store)->FlushAndWait());
        CHECK(std::filesystem::exists(slot.Dir / "slow.1.vst"));

        // Inside the interval the slow family stays dirty and the commit keeps its old file.
        now = 1.0;
        (*store)->Write(FastFamily, StoreKey{.Lo = 1}, MakeRecord(3));
        (*store)->Write(SlowFamily, StoreKey{.Lo = 2}, MakeRecord(4));
        (*store)->Flush();
        CHECK((*store)->GetGeneration() == 2);
        CHECK((*store)->IsDirty());
        CHECK(std::filesystem::exists(slot.Dir / "fast.2.vst"));
        CHECK(std::filesystem::exists(slot.Dir / "slow.1.vst"));
        CHECK(!std::filesystem::exists(slot.Dir / "slow.2.vst"));
        CopySlot(slot.Dir, copy.Dir);

        now = 9.5;
        (*store)->Flush();
        CHECK((*store)->GetGeneration() == 2);

        // Once the interval has passed since the flush that last took it, the next flush does.
        now = 10.0;
        (*store)->Flush();
        CHECK((*store)->GetGeneration() == 3);
        CHECK(std::filesystem::exists(slot.Dir / "slow.3.vst"));
        CHECK(!(*store)->IsDirty());

        // A full flush writes it whatever its interval.
        now = 11.0;
        (*store)->Write(SlowFamily, StoreKey{.Lo = 2}, MakeRecord(5));
        REQUIRE((*store)->FlushAndWait());
        CHECK((*store)->GetGeneration() == 4);
        CHECK(!(*store)->IsDirty());
    }

    Result<Unique<Store>> held = Store::Open(copy.Dir);
    REQUIRE(held);
    CHECK(ValueOf(**held, FastFamily, 1) == 3);
    CHECK(ValueOf(**held, SlowFamily, 2) == 2);

    Result<Unique<Store>> reopened = Store::Open(slot.Dir);
    REQUIRE(reopened);
    CHECK(ValueOf(**reopened, SlowFamily, 2) == 5);
}

TEST_CASE("closing a store commits a write in flight first")
{
    SUBCASE("a job the executor never ran is drained by the close")
    {
        const TempSlot slot;
        HeldWrites held;
        {
            Result<Unique<Store>> store = Store::Open(slot.Dir, held.Info());
            REQUIRE(store);
            RegisterFamilies(**store);
            (*store)->Write(FastFamily, StoreKey{.Lo = 1}, MakeRecord(7));
            (*store)->Flush();
            REQUIRE(held.Jobs.size() == 1);
        }
        // Run after the close drained it: the job finds nothing left and touches only its own
        // shared state.
        held.RunAll();

        Result<Unique<Store>> reopened = Store::Open(slot.Dir);
        REQUIRE(reopened);
        CHECK((*reopened)->GetGeneration() == 1);
        CHECK(ValueOf(**reopened, FastFamily, 1) == 7);
    }

    SUBCASE("a write on the ambient task pool")
    {
        const TempSlot slot;
        {
            TaskSystem pool(TaskSystemInfo{.WorkerCount = 1});
            pool.SetAmbientForCurrentThread();
            Result<Unique<Store>> store = Store::Open(slot.Dir);
            REQUIRE(store);
            RegisterFamilies(**store);
            (*store)->Write(FastFamily, StoreKey{.Lo = 1}, MakeRecord(8));
            (*store)->Flush();
        }

        Result<Unique<Store>> reopened = Store::Open(slot.Dir);
        REQUIRE(reopened);
        CHECK(ValueOf(**reopened, FastFamily, 1) == 8);
    }
}
