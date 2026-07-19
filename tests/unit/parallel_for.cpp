// ParallelFor unit cases: a pure-CPU range-splitting utility, no GPU, no Context.
// Proves every index in [0, count) is visited exactly once across the split, that
// disjoint output writes need no synchronization, and the degenerate ranges (empty,
// single-threaded, count below the thread bound).

#include <doctest/doctest.h>

#include <atomic>
#include <numeric>

#include <Veng/Task/ParallelFor.h>

using namespace Veng;

TEST_CASE("ParallelFor visits every index exactly once")
{
    for (const usize count : {usize{1}, usize{7}, usize{64}, usize{1000}, usize{4097}})
    {
        vector<u32> visits(count, 0u);
        ParallelFor(count,
                    [&](usize begin, usize end)
                    {
                        for (usize i = begin; i < end; ++i)
                        {
                            // Disjoint index ranges write disjoint slots, so no synchronization is needed.
                            ++visits[i];
                        }
                    });

        for (const u32 v : visits)
        {
            REQUIRE(v == 1u);
        }
    }
}

TEST_CASE("ParallelFor covers the whole range with a parallel reduction")
{
    constexpr usize Count = 10000;
    std::atomic<u64> total{0};
    ParallelFor(Count,
                [&](usize begin, usize end)
                {
                    u64 partial = 0;
                    for (usize i = begin; i < end; ++i)
                    {
                        partial += i;
                    }
                    total.fetch_add(partial, std::memory_order_relaxed);
                });

    // 0 + 1 + ... + (Count-1).
    REQUIRE(total.load() == static_cast<u64>(Count) * (Count - 1) / 2);
}

TEST_CASE("ParallelFor with an empty range runs the body not at all")
{
    u32 calls = 0;
    ParallelFor(0, [&](usize, usize) { ++calls; });
    REQUIRE(calls == 0u);
}

TEST_CASE("ParallelFor with maxThreads == 1 runs inline as a single range")
{
    constexpr usize Count = 500;
    u32 rangeCount = 0;
    usize seenBegin = 1;
    usize seenEnd = 0;
    ParallelFor(
        Count,
        [&](usize begin, usize end)
        {
            ++rangeCount;
            seenBegin = begin;
            seenEnd = end;
        },
        1u);

    REQUIRE(rangeCount == 1u);
    REQUIRE(seenBegin == 0u);
    REQUIRE(seenEnd == Count);
}

TEST_CASE("ParallelFor caps concurrency at the index count")
{
    // Three indices cannot be split into more than three ranges however many threads are asked
    // for, so each observed range is non-empty and the union is still exact.
    constexpr usize Count = 3;
    std::atomic<u32> rangeCount{0};
    vector<u32> visits(Count, 0u);
    ParallelFor(
        Count,
        [&](usize begin, usize end)
        {
            REQUIRE(end > begin);
            rangeCount.fetch_add(1, std::memory_order_relaxed);
            for (usize i = begin; i < end; ++i)
            {
                ++visits[i];
            }
        },
        64u);

    REQUIRE(rangeCount.load() <= Count);
    for (const u32 v : visits)
    {
        REQUIRE(v == 1u);
    }
}
