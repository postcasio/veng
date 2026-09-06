// ParallelFor's pool-routing cases: when a TaskSystem pool is ambient, ParallelFor dispatches
// onto it (spawning zero threads) with the caller participating, and falls back to owning its
// own threads only off a veng-spawned thread. These assert the routing's properties — no threads
// added, liveness under a saturated pool, callable from a worker incl. nested, and identical
// per-index output on both paths. The fallback path's own correctness is in parallel_for.cpp.

#include <doctest/doctest.h>

#include <atomic>
#include <thread>

#include <Veng/Task/ParallelFor.h>
#include <Veng/Task/TaskSystem.h>

using namespace Veng;

TEST_CASE("ParallelFor routed through the ambient pool spawns no threads")
{
    TaskSystem pool;

    constexpr usize Count = 4096;
    vector<u32> visits(Count, 0u);

    // Running from a worker makes the pool ambient, so ParallelFor routes onto it. The spawn
    // counter is monotonic and process-wide, so the property is that it does not move across the
    // call — asserted deterministically rather than by sampling a live thread count.
    const u64 before = Detail::GetParallelForThreadSpawnCount();
    (void)pool
        .Submit(
            [&]
            {
                ParallelFor(Count,
                            [&](usize begin, usize end)
                            {
                                for (usize i = begin; i < end; ++i)
                                {
                                    ++visits[i];
                                }
                            });
            })
        .Get();
    const u64 after = Detail::GetParallelForThreadSpawnCount();

    CHECK(after == before);
    for (const u32 v : visits)
    {
        CHECK(v == 1u);
    }
}

TEST_CASE("ParallelFor on the main thread completes even with every worker saturated")
{
    TaskSystem pool;
    // Bind the pool as this thread's ambient pool, exactly as Application does for main, so a
    // ParallelFor here participates through the pool.
    pool.SetAmbientForCurrentThread();

    const u32 workers = pool.GetWorkerCount();
    std::atomic<u32> occupied{0};
    std::atomic<bool> release{false};

    vector<Task<void>> hogs;
    hogs.reserve(workers);
    for (u32 i = 0; i < workers; ++i)
    {
        hogs.push_back(pool.Submit(
            [&]
            {
                occupied.fetch_add(1, std::memory_order_relaxed);
                while (!release.load(std::memory_order_acquire))
                {
                    std::this_thread::yield();
                }
            }));
    }

    // Every worker is now parked in a hog job, so no helper can run.
    while (occupied.load(std::memory_order_relaxed) < workers)
    {
        std::this_thread::yield();
    }

    // Liveness: the caller's own claim-loop must cover every range with no worker free — reaching
    // the assertions at all is the property, and full coverage confirms the work was done.
    constexpr usize Count = 2048;
    vector<u32> visits(Count, 0u);
    ParallelFor(Count,
                [&](usize begin, usize end)
                {
                    for (usize i = begin; i < end; ++i)
                    {
                        ++visits[i];
                    }
                });

    for (const u32 v : visits)
    {
        CHECK(v == 1u);
    }

    release.store(true, std::memory_order_release);
    for (Task<void>& hog : hogs)
    {
        (void)hog.Get();
    }
}

TEST_CASE("ParallelFor is callable from a pool worker, including nested")
{
    TaskSystem pool;

    constexpr usize Outer = 64;
    constexpr usize Inner = 32;
    vector<u32> visits(Outer * Inner, 0u);

    (void)pool
        .Submit(
            [&]
            {
                ParallelFor(Outer,
                            [&](usize outerBegin, usize outerEnd)
                            {
                                for (usize o = outerBegin; o < outerEnd; ++o)
                                {
                                    ParallelFor(Inner,
                                                [&](usize innerBegin, usize innerEnd)
                                                {
                                                    for (usize i = innerBegin; i < innerEnd; ++i)
                                                    {
                                                        ++visits[o * Inner + i];
                                                    }
                                                });
                                }
                            });
            })
        .Get();

    for (const u32 v : visits)
    {
        CHECK(v == 1u);
    }
}

TEST_CASE("ParallelFor gives identical per-index output on the pool path and the fallback")
{
    constexpr usize Count = 5000;
    const auto fill = [](vector<u64>& out)
    {
        ParallelFor(out.size(),
                    [&](usize begin, usize end)
                    {
                        for (usize i = begin; i < end; ++i)
                        {
                            // A per-index write: disjoint ranges write disjoint slots, so the
                            // result is order-independent and must agree byte-for-byte across the
                            // two paths (unlike a non-associative float reduction).
                            out[i] = static_cast<u64>(i) * 3u + 1u;
                        }
                    });
    };

    // No pool ambient on the test's main thread → the fallback spawn-own-threads path.
    vector<u64> fallback(Count, 0u);
    fill(fallback);

    // Ambient pool on a worker → the pool path.
    vector<u64> pooled(Count, 0u);
    {
        TaskSystem pool;
        (void)pool.Submit([&] { fill(pooled); }).Get();
    }

    CHECK(fallback == pooled);
    for (usize i = 0; i < Count; ++i)
    {
        CHECK(fallback[i] == static_cast<u64>(i) * 3u + 1u);
    }
}
