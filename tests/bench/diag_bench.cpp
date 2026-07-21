// Diagnostics profiler microbenchmark: the first per-scope cost measurement against the plan's
// ≤ 40 ns budget (recording on, including the release store), the per-frame aggregation-fold cost
// measured separately, and a hard allocation-free check on the steady-state hot path.
//
// Timings are informational and printed to stdout; the process exit status asserts only the
// deterministic invariant — that the measured steady-state scope loop allocates nothing. Built only
// under VE_PROFILE (there is nothing to measure with the macros compiled out).

#include <atomic>
#include <chrono>
#include <cstdio>
#include <cstdlib>
#include <new>

#include <Veng/Diagnostics/Profiler.h>

namespace
{
    std::atomic<Veng::u64> g_Allocations{0};
    std::atomic<bool> g_CountAllocations{false};

    void CountAllocation()
    {
        if (g_CountAllocations.load(std::memory_order_relaxed))
        {
            g_Allocations.fetch_add(1, std::memory_order_relaxed);
        }
    }
}

void* operator new(std::size_t size)
{
    CountAllocation();
    void* p = std::malloc(size == 0 ? 1 : size);
    return p;
}
void* operator new[](std::size_t size)
{
    CountAllocation();
    void* p = std::malloc(size == 0 ? 1 : size);
    return p;
}
void operator delete(void* p) noexcept
{
    std::free(p);
}
void operator delete[](void* p) noexcept
{
    std::free(p);
}
void operator delete(void* p, std::size_t) noexcept
{
    std::free(p);
}
void operator delete[](void* p, std::size_t) noexcept
{
    std::free(p);
}

using namespace Veng;
using namespace Veng::Diagnostics;
using Clock = std::chrono::steady_clock;

namespace
{
    f64 NanosPer(u64 elapsedNanos, u64 count)
    {
        return static_cast<f64>(elapsedNanos) / count;
    }

    u64 ElapsedNanos(Clock::time_point begin, Clock::time_point end)
    {
        return static_cast<u64>(
            std::chrono::duration_cast<std::chrono::nanoseconds>(end - begin).count());
    }
}

int main()
{
    constexpr u64 ScopeIterations = 2'000'000;
    constexpr u64 FrameIterations = 100'000;

    // A large ring so the recording loop never hands to a sink and rarely re-arms a chunk: what we
    // measure is the append plus the release store, which is the budgeted hot path.
    ProfilerConfig config;
    config.ChunkBytes = 8u * 1024u * 1024u;
    config.ChunksPerThread = 2;

    // --- Recording-on per-scope cost (append + release store + aggregation) ------------------
    f64 recordingNs = 0.0;
    {
        Profiler profiler(config);
        profiler.SetMode(ProfilerMode::Ring); // recording, null sink: ring drain, no hand-off

        // Warm up: intern the literal, prime the aggregate node and the first chunk.
        for (u32 i = 0; i < 1000; ++i)
        {
            VE_PROFILE_SCOPE("Bench");
        }
        profiler.BeginFrame();

        g_Allocations.store(0, std::memory_order_relaxed);
        g_CountAllocations.store(true, std::memory_order_relaxed);
        const Clock::time_point begin = Clock::now();
        for (u64 i = 0; i < ScopeIterations; ++i)
        {
            VE_PROFILE_SCOPE("Bench");
        }
        const Clock::time_point end = Clock::now();
        g_CountAllocations.store(false, std::memory_order_relaxed);

        recordingNs = NanosPer(ElapsedNanos(begin, end), ScopeIterations);
        const u64 allocations = g_Allocations.load(std::memory_order_relaxed);

        std::printf("recording-on per-scope:   %.1f ns/scope  (budget 40 ns)\n", recordingNs);
        std::printf("hot-path allocations:     %llu\n",
                    static_cast<unsigned long long>(allocations));

        if (allocations != 0)
        {
            std::printf("FAIL: the steady-state scope loop allocated %llu times\n",
                        static_cast<unsigned long long>(allocations));
            return 1;
        }
    }

    // --- Aggregation-only per-scope cost (Mode Off: aggregate, no buffer write) ---------------
    {
        Profiler profiler(config); // Off mode: aggregation still runs, no recording
        for (u32 i = 0; i < 1000; ++i)
        {
            VE_PROFILE_SCOPE("Bench");
        }
        profiler.BeginFrame();

        const Clock::time_point begin = Clock::now();
        for (u64 i = 0; i < ScopeIterations; ++i)
        {
            VE_PROFILE_SCOPE("Bench");
        }
        const Clock::time_point end = Clock::now();
        std::printf("aggregation-only per-scope: %.1f ns/scope\n",
                    NanosPer(ElapsedNanos(begin, end), ScopeIterations));
    }

    // --- Per-frame aggregation fold cost -----------------------------------------------------
    {
        Profiler profiler(config);
        // Populate a realistic spread of distinct scopes so the fold walks a non-trivial map.
        const char* names[] = {"PhaseA", "PhaseB", "PhaseC", "PhaseD",
                               "PhaseE", "PhaseF", "PhaseG", "PhaseH"};
        for (const char* name : names)
        {
            (void)profiler.InternName(name);
        }
        for (u32 i = 0; i < 8; ++i)
        {
            VE_PROFILE_SCOPE_DYNAMIC(string(names[i]));
        }
        profiler.BeginFrame();

        const Clock::time_point begin = Clock::now();
        for (u64 i = 0; i < FrameIterations; ++i)
        {
            for (u32 j = 0; j < 8; ++j)
            {
                VE_PROFILE_SCOPE_DYNAMIC(string(names[j]));
            }
            profiler.BeginFrame();
        }
        const Clock::time_point end = Clock::now();
        std::printf("per-frame aggregation fold: %.1f ns/frame (8 scopes/frame)\n",
                    NanosPer(ElapsedNanos(begin, end), FrameIterations));
    }

    std::printf("PASS: hot path is allocation-free\n");
    return 0;
}
