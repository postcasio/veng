#include <Veng/Task/ParallelFor.h>

#include <Veng/Diagnostics/Profiler.h>

#include <algorithm>
#include <thread>

namespace Veng
{
    void ParallelFor(usize count, const function<void(usize begin, usize end)>& body,
                     u32 maxThreads)
    {
        if (count == 0)
        {
            return;
        }

        VE_PROFILE_SCOPE("ParallelFor");

        u32 requested = maxThreads;
        if (requested == 0)
        {
            requested = std::thread::hardware_concurrency();
            // hardware_concurrency is allowed to report 0 when it cannot tell; fall back to serial.
            if (requested == 0)
            {
                requested = 1;
            }
        }

        const usize threads = std::min<usize>(requested, count);
        if (threads <= 1)
        {
            VE_PROFILE_SCOPE("ParallelFor/Range");
            body(0, count);
            return;
        }

        // Contiguous near-equal split: the first (count % threads) ranges take one extra index so
        // every index is covered exactly once and range sizes differ by at most one.
        const usize base = count / threads;
        const usize remainder = count % threads;

        vector<std::thread> helpers;
        helpers.reserve(threads - 1);

        usize begin = 0;
        for (usize i = 0; i < threads; ++i)
        {
            const usize end = begin + base + (i < remainder ? 1u : 0u);
            // The final range runs on the calling thread, so a body capturing thread-local or
            // caller-stack context still sees it for that share, and one fewer thread is spawned.
            if (i + 1 == threads)
            {
                VE_PROFILE_SCOPE("ParallelFor/Range");
                body(begin, end);
            }
            else
            {
                helpers.emplace_back(
                    [&body, begin, end]
                    {
                // A transient helper is not a pool worker, so it registers its own track
                // explicitly; the RAII handle flushes and unlinks it at thread exit. A run
                // that cycles more identities than the profiler's max is accounted, not
                // silently dropped.
#if defined(VE_PROFILE) && VE_PROFILE
                        Diagnostics::ProfilerThreadRegistration registration;
                        if (Diagnostics::Profiler* profiler = Diagnostics::GetActiveProfiler())
                        {
                            registration = profiler->RegisterThread("ParallelFor");
                        }
#endif
                        VE_PROFILE_SCOPE("ParallelFor/Range");
                        body(begin, end);
                    });
            }
            begin = end;
        }

        for (std::thread& helper : helpers)
        {
            helper.join();
        }
    }
}
