// TaskSystem / Task<T> unit cases: a pure-CPU threading subsystem, no GPU, no
// Context. Proves result delivery, the Result<T> error channel (no exceptions),
// and that Then continuations run only during PumpMainThread and only on the
// pumping thread.

#include <doctest/doctest.h>

#include <atomic>
#include <thread>

#include <Veng/Task/TaskSystem.h>

using namespace Veng;

TEST_CASE("Submit N independent jobs; all results correct")
{
    TaskSystem tasks;

    constexpr u32 Count = 64;
    vector<Task<int>> handles;
    handles.reserve(Count);
    for (u32 i = 0; i < Count; ++i)
    {
        handles.push_back(tasks.Submit([i] { return static_cast<int>(i * i); }));
    }

    for (u32 i = 0; i < Count; ++i)
    {
        Result<int> r = handles[i].Get();
        REQUIRE(r.has_value());
        CHECK(*r == static_cast<int>(i * i));
    }
}

TEST_CASE("A job returning an error Result<T> surfaces it through Get (no throw)")
{
    TaskSystem tasks;

    Task<int> ok = tasks.Submit([] { return Result<int>(7); });
    Task<int> bad = tasks.Submit([] { return Result<int>(std::unexpected("boom")); });

    Result<int> okResult = ok.Get();
    REQUIRE(okResult.has_value());
    CHECK(*okResult == 7);

    Result<int> badResult = bad.Get();
    CHECK_FALSE(badResult.has_value());
    CHECK(badResult.error() == "boom");
}

TEST_CASE("void jobs complete and report success")
{
    TaskSystem tasks;

    std::atomic<int> counter = 0;
    Task<void> t = tasks.Submit([&counter] { counter.fetch_add(1); });

    Result<std::monostate> r = t.Get();
    CHECK(r.has_value());
    CHECK(counter.load() == 1);
}

TEST_CASE("Then continuations run only during PumpMainThread, only on the pumping thread")
{
    TaskSystem tasks;

    const std::thread::id pumpThread = std::this_thread::get_id();

    std::atomic<bool> ran = false;
    std::atomic<bool> ranOnPumpThread = false;
    int delivered = 0;

    Task<int> t = tasks.Submit([] { return 99; });
    t.Then(
        [&](Result<int> r)
        {
            ran.store(true);
            ranOnPumpThread.store(std::this_thread::get_id() == pumpThread);
            if (r.has_value())
            {
                delivered = *r;
            }
        });

    // The job has run (we can prove it), but the continuation must NOT have:
    // it only fires inside PumpMainThread.
    (void)t.Get();
    CHECK_FALSE(ran.load());

    tasks.PumpMainThread();
    CHECK(ran.load());
    CHECK(ranOnPumpThread.load());
    CHECK(delivered == 99);
}

TEST_CASE("Then registered after completion still defers to PumpMainThread")
{
    TaskSystem tasks;

    Task<int> t = tasks.Submit([] { return 5; });
    (void)t.Get(); // ensure the result has already landed

    std::atomic<bool> ran = false;
    t.Then([&](Result<int>) { ran.store(true); });

    CHECK_FALSE(ran.load());
    tasks.PumpMainThread();
    CHECK(ran.load());
}

TEST_CASE("WaitForAll blocks until every in-flight job is done")
{
    TaskSystem tasks;

    constexpr u32 Count = 32;
    std::atomic<u32> completed = 0;
    for (u32 i = 0; i < Count; ++i)
    {
        tasks.Submit(
            [&completed]
            {
                std::this_thread::sleep_for(std::chrono::milliseconds(2));
                completed.fetch_add(1);
            });
    }

    tasks.WaitForAll();
    CHECK(completed.load() == Count);
}

TEST_CASE("WorkerCount = 0 derives a sane count (>= 1)")
{
    TaskSystem tasks(TaskSystemInfo{.WorkerCount = 0});
    CHECK(tasks.GetWorkerCount() >= 1);
}

TEST_CASE("WorkerCount is honored when set explicitly")
{
    TaskSystem tasks(TaskSystemInfo{.WorkerCount = 3});
    CHECK(tasks.GetWorkerCount() == 3);
}

TEST_CASE("ForEachWorker runs once on each worker")
{
    TaskSystem tasks(TaskSystemInfo{.WorkerCount = 4});

    std::mutex mutex;
    set<u32> seen;
    std::atomic<u32> calls = 0;

    tasks.ForEachWorker(
        [&](u32 index)
        {
            calls.fetch_add(1);
            std::lock_guard lock(mutex);
            seen.insert(index);
        });

    CHECK(calls.load() == 4);
    CHECK(seen.size() == 4); // distinct indices: one run per worker
}

TEST_CASE("Stress: many small jobs across the pool")
{
    TaskSystem tasks;

    constexpr u32 Count = 4000;
    vector<Task<u64>> handles;
    handles.reserve(Count);
    for (u32 i = 0; i < Count; ++i)
    {
        handles.push_back(tasks.Submit([i] { return static_cast<u64>(i) + 1; }));
    }

    u64 sum = 0;
    for (Task<u64>& h : handles)
    {
        Result<u64> r = h.Get();
        REQUIRE(r.has_value());
        sum += *r;
    }

    // Sum of 1..Count.
    CHECK(sum == static_cast<u64>(Count) * (Count + 1) / 2);
}
