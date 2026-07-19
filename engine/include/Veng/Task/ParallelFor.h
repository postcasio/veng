#pragma once

#include <Veng/Veng.h>

namespace Veng
{
    /// @brief Runs a body over [0, count) split into contiguous ranges across several threads,
    ///        blocking until every range has finished.
    ///
    /// The range is divided into up to min(count, maxThreads) near-equal contiguous sub-ranges,
    /// and body(begin, end) is invoked once per sub-range. The calling thread runs one of the
    /// sub-ranges itself and spawns a short-lived thread for each of the others, so a
    /// single-range split runs fully inline with nothing spawned. All spawned threads are joined
    /// before the call returns.
    ///
    /// This owns its threads for the call's duration rather than drawing on the TaskSystem pool,
    /// which makes it correct to call from any thread — including a TaskSystem worker, where
    /// re-entering the pool could starve it — and suits coarse, occasional, CPU-bound batch work
    /// (a one-shot bake, a bulk transform). It is not for per-frame hot paths: the thread setup is
    /// only amortized when count is large and the call infrequent. For steady per-frame work,
    /// submit to the TaskSystem instead.
    ///
    /// @param count       Number of indices to cover; a count of 0 runs body not at all.
    /// @param body        Invoked as body(begin, end) per sub-range, concurrently from multiple
    ///                    threads. It must not touch shared mutable state without synchronization,
    ///                    and — like any off-main-thread work — must not call into veng APIs that
    ///                    require the main thread.
    /// @param maxThreads  Upper bound on concurrency; 0 derives it from the hardware.
    /// @warning body runs on threads other than the caller. Data races in body are the caller's
    ///          responsibility; disjoint index ranges writing to disjoint outputs need none.
    VE_API void ParallelFor(usize count, const function<void(usize begin, usize end)>& body,
                            u32 maxThreads = 0);
}
