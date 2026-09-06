#pragma once

#include <Veng/Veng.h>

namespace Veng
{
    /// @brief Runs a body over [0, count) split into contiguous ranges, blocking until every
    ///        range has finished.
    ///
    /// The range is divided into near-equal contiguous sub-ranges and body(begin, end) is invoked
    /// once per sub-range, concurrently. When a TaskSystem pool is ambient on the calling thread
    /// (true on every worker and on the main thread), the split runs on that pool through
    /// TaskSystem::RunParallel with the caller participating — no threads are spawned, and total
    /// concurrency stays bounded to the pool plus the caller, so it cannot oversubscribe the
    /// machine. Off a veng-spawned thread — a unit test, an external std::thread — no pool is
    /// ambient and it falls back to spawning up to min(count, maxThreads) short-lived threads it
    /// owns for the call and joins before returning.
    ///
    /// Because the caller participates, it stays safe to call from a TaskSystem worker: the
    /// caller's own claim-loop completes every range even if no helper runs, so a call from a
    /// worker (or a nested call) cannot starve or deadlock the pool. Concurrency is
    /// bounded/best-effort — when the workers are busy the caller runs the ranges itself, and no
    /// range is guaranteed a distinct thread. It suits coarse, occasional, CPU-bound batch work (a
    /// one-shot bake, a bulk transform), not per-frame hot paths; for steady per-frame work, submit
    /// to the TaskSystem directly.
    ///
    /// @param count       Number of indices to cover; a count of 0 runs body not at all.
    /// @param body        Invoked as body(begin, end) per sub-range, concurrently from multiple
    ///                    threads. It must not touch shared mutable state without synchronization,
    ///                    and — like any off-main-thread work — must not call into veng APIs that
    ///                    require the main thread.
    /// @param maxThreads  Upper bound on the range count; 0 derives it from the pool size (or, on
    ///                    the fallback path, from the hardware).
    /// @warning body runs on threads other than the caller. Data races in body are the caller's
    ///          responsibility; disjoint index ranges writing to disjoint outputs need none.
    VE_API void ParallelFor(usize count, const function<void(usize begin, usize end)>& body,
                            u32 maxThreads = 0);

    namespace Detail
    {
        /// @brief Test seam: cumulative count of std::threads ParallelFor has spawned.
        ///
        /// Incremented once per thread spawned on the fallback path and never on the ambient-pool
        /// path, so a test asserts the pool routing added no threads by checking the count did not
        /// move across a call. Process-wide and monotonic.
        [[nodiscard]] VE_API u64 GetParallelForThreadSpawnCount();
    }
}
