// TaskSystem / Task<T> unit cases: a pure-CPU threading subsystem, no GPU, no
// Context. Proves result delivery, the Result<T> error channel (no exceptions),
// and that Then continuations run only during PumpMainThread and only on the
// pumping thread.

#include <doctest/doctest.h>

#include <algorithm>
#include <atomic>
#include <condition_variable>
#include <mutex>
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

    const Result<std::monostate> r = t.Get();
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
    const TaskSystem tasks(TaskSystemInfo{.WorkerCount = 0});
    CHECK(tasks.GetWorkerCount() >= 1);
}

TEST_CASE("WorkerCount is honored when set explicitly")
{
    const TaskSystem tasks(TaskSystemInfo{.WorkerCount = 3});
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
            const std::scoped_lock lock(mutex);
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

// --- RunParallel -------------------------------------------------------------

TEST_CASE("RunParallel visits every index exactly once across a spread of splits")
{
    TaskSystem tasks;

    for (const usize count : {usize{1}, usize{2}, usize{7}, usize{64}, usize{1000}, usize{4097}})
    {
        // maxRanges 0 derives the pool-sized default; the rest exercise a single range, a
        // two-way split, and a bound above the index count (which clamps to count).
        for (const u32 maxRanges : {0u, 1u, 2u, 4u, 64u})
        {
            vector<u32> visits(count, 0u);
            tasks.RunParallel(
                count,
                [&](usize begin, usize end)
                {
                    for (usize i = begin; i < end; ++i)
                    {
                        // Disjoint ranges write disjoint slots, so no synchronization is needed.
                        ++visits[i];
                    }
                },
                maxRanges);

            const bool everyIndexOnce =
                std::ranges::all_of(visits, [](const u32 v) { return v == 1u; });
            CHECK(everyIndexOnce);
        }
    }
}

TEST_CASE("RunParallel with an empty range runs the body not at all")
{
    TaskSystem tasks;

    std::atomic<u32> calls{0};
    tasks.RunParallel(0, [&](usize, usize) { calls.fetch_add(1); });
    CHECK(calls.load() == 0u);
}

TEST_CASE("RunParallel is deadlock-safe when called from a pool worker, including nested")
{
    // A non-participating "submit sub-ranges then block" design deadlocks at these worker counts;
    // the caller running the claim-loop inline is what keeps it live.
    for (const u32 workers : {1u, 2u})
    {
        TaskSystem tasks(TaskSystemInfo{.WorkerCount = workers});

        constexpr usize N = 1000;
        std::atomic<u64> sum{0};
        Task<void> outer = tasks.Submit(
            [&]
            {
                tasks.RunParallel(N,
                                  [&](usize begin, usize end)
                                  {
                                      u64 partial = 0;
                                      for (usize i = begin; i < end; ++i)
                                      {
                                          partial += i;
                                      }
                                      sum.fetch_add(partial, std::memory_order_relaxed);
                                  });
            });
        (void)outer.Get();
        CHECK(sum.load() == static_cast<u64>(N) * (N - 1) / 2);

        // A RunParallel whose body itself calls RunParallel: the inner dispatch runs from a worker
        // already inside an outer dispatch, the case a pool-recursing design starves.
        constexpr usize Outer = 8;
        constexpr usize Inner = 100;
        std::atomic<u64> nested{0};
        Task<void> nestedOuter = tasks.Submit(
            [&]
            {
                tasks.RunParallel(
                    Outer,
                    [&](usize ob, usize oe)
                    {
                        for (usize o = ob; o < oe; ++o)
                        {
                            tasks.RunParallel(
                                Inner, [&](usize b, usize e)
                                { nested.fetch_add(e - b, std::memory_order_relaxed); });
                        }
                    });
            });
        (void)nestedOuter.Get();
        CHECK(nested.load() == static_cast<u64>(Outer) * Inner);
    }
}

TEST_CASE("RunParallel adds no threads: body runs only on pool workers or the caller")
{
    TaskSystem tasks(TaskSystemInfo{.WorkerCount = 4});

    std::mutex mutex;
    set<std::thread::id> workerIds;
    tasks.ForEachWorker(
        [&](u32)
        {
            const std::scoped_lock lock(mutex);
            workerIds.insert(std::this_thread::get_id());
        });
    const std::thread::id callerId = std::this_thread::get_id();

    set<std::thread::id> bodyIds;
    tasks.RunParallel(100000,
                      [&](usize begin, usize end)
                      {
                          {
                              const std::scoped_lock lock(mutex);
                              bodyIds.insert(std::this_thread::get_id());
                          }
                          // A little work so ranges genuinely land on helpers, not just the caller.
                          volatile u64 acc = 0;
                          for (usize i = begin; i < end; ++i)
                          {
                              acc += i;
                          }
                          (void)acc;
                      });

    // Every thread that ran a range is either the caller or one of the existing workers — had
    // RunParallel spawned a thread, an unknown id would appear here.
    const bool onlyPoolOrCaller =
        std::ranges::all_of(bodyIds, [&](const std::thread::id id)
                            { return id == callerId || workerIds.contains(id); });
    CHECK(onlyPoolOrCaller);
}

TEST_CASE("RunParallel completes on the caller alone when every worker is occupied")
{
    TaskSystem tasks(TaskSystemInfo{.WorkerCount = 4});
    const u32 workers = tasks.GetWorkerCount();

    // Park every worker so no helper job can run; the caller's inline claim-loop must still
    // complete all ranges. If it depended on a helper, this would hang (caught by the ctest timeout).
    std::mutex mutex;
    std::condition_variable release;
    std::atomic<u32> parked{0};
    std::atomic<bool> go{false};
    for (u32 i = 0; i < workers; ++i)
    {
        tasks.Submit(
            [&]
            {
                parked.fetch_add(1);
                std::unique_lock lock(mutex);
                release.wait(lock, [&] { return go.load(); });
            });
    }
    while (parked.load() < workers)
    {
        std::this_thread::yield();
    }

    vector<u32> visits(500, 0u);
    tasks.RunParallel(500,
                      [&](usize begin, usize end)
                      {
                          for (usize i = begin; i < end; ++i)
                          {
                              ++visits[i];
                          }
                      });
    const bool everyIndexOnce = std::ranges::all_of(visits, [](const u32 v) { return v == 1u; });
    CHECK(everyIndexOnce);

    {
        const std::scoped_lock lock(mutex);
        go.store(true);
    }
    release.notify_all();
    tasks.WaitForAll();
}

TEST_CASE("RunParallel stragglers running after it returns touch only live state")
{
    // The helper jobs are parked behind occupied workers, so they first run after RunParallel has
    // already returned. With stack-owned claim-state this is a use-after-scope (a sanitiser build
    // is where it bites); heap co-ownership keeps the state alive until the last helper drops it.
    TaskSystem tasks(TaskSystemInfo{.WorkerCount = 4});
    const u32 workers = tasks.GetWorkerCount();

    std::mutex mutex;
    std::condition_variable release;
    std::atomic<u32> parked{0};
    std::atomic<bool> go{false};
    for (u32 i = 0; i < workers; ++i)
    {
        tasks.Submit(
            [&]
            {
                parked.fetch_add(1);
                std::unique_lock lock(mutex);
                release.wait(lock, [&] { return go.load(); });
            });
    }
    while (parked.load() < workers)
    {
        std::this_thread::yield();
    }

    std::atomic<u64> sum{0};
    constexpr usize N = 4096;
    tasks.RunParallel(N,
                      [&](usize begin, usize end)
                      {
                          u64 partial = 0;
                          for (usize i = begin; i < end; ++i)
                          {
                              partial += i;
                          }
                          sum.fetch_add(partial, std::memory_order_relaxed);
                      });
    CHECK(sum.load() == static_cast<u64>(N) * (N - 1) / 2);

    // Release the parked workers; the queued helpers now run as stragglers, find the cursor
    // exhausted, and unwind against the still-live shared state.
    {
        const std::scoped_lock lock(mutex);
        go.store(true);
    }
    release.notify_all();
    tasks.WaitForAll();
}

TEST_CASE("The ambient pool resolves on a worker and is null off veng-spawned threads")
{
    TaskSystem tasks(TaskSystemInfo{.WorkerCount = 2});

    Task<TaskSystem*> onWorker = tasks.Submit([] { return TaskSystem::GetAmbientPool(); });
    const Result<TaskSystem*> resolved = onWorker.Get();
    REQUIRE(resolved.has_value());
    CHECK(*resolved == &tasks);

    std::atomic<bool> nullOnSpawnedThread{false};
    std::thread external([&]
                         { nullOnSpawnedThread.store(TaskSystem::GetAmbientPool() == nullptr); });
    external.join();
    CHECK(nullOnSpawnedThread.load());

    // The unit-test main thread never called SetAmbientForCurrentThread (only Application does),
    // so it reads null — the fallback signal a non-veng thread keys on.
    CHECK(TaskSystem::GetAmbientPool() == nullptr);
}
