#include <Veng/SystemStats.h>

#if defined(_WIN32)
#include <windows.h>
#include <psapi.h>
#elif defined(__APPLE__)
#include <libproc.h>
#include <sys/sysctl.h>
#include <unistd.h>
#else
#include <cstdio>
#include <sys/resource.h>
#include <unistd.h>
#endif

namespace Veng
{
    namespace
    {
        /// @brief Reads this process's consumed CPU time in nanoseconds, 0 when unavailable.
        u64 ProcessCpuNanos()
        {
#if defined(_WIN32)
            FILETIME creation{};
            FILETIME exit{};
            FILETIME kernel{};
            FILETIME user{};
            if (GetProcessTimes(GetCurrentProcess(), &creation, &exit, &kernel, &user) == 0)
            {
                return 0;
            }
            // Kernel and user time are each 100-ns tick counts split across two 32-bit words.
            const auto ticks = [](const FILETIME& time)
            { return (static_cast<u64>(time.dwHighDateTime) << 32) | time.dwLowDateTime; };
            return (ticks(kernel) + ticks(user)) * 100;
#elif defined(__APPLE__)
            rusage_info_current info{};
            if (proc_pid_rusage(getpid(), RUSAGE_INFO_CURRENT,
                                reinterpret_cast<rusage_info_t*>(&info)) != 0)
            {
                return 0;
            }
            // ri_user_time / ri_system_time are already nanoseconds.
            return info.ri_user_time + info.ri_system_time;
#else
            rusage usage{};
            if (getrusage(RUSAGE_SELF, &usage) != 0)
            {
                return 0;
            }
            const auto toNanos = [](const timeval& time)
            {
                return static_cast<u64>(time.tv_sec) * 1'000'000'000ULL +
                       static_cast<u64>(time.tv_usec) * 1'000ULL;
            };
            return toNanos(usage.ru_utime) + toNanos(usage.ru_stime);
#endif
        }

        /// @brief Reads this process's resident/footprint memory in bytes, 0 when unavailable.
        u64 ProcessMemoryBytes()
        {
#if defined(_WIN32)
            PROCESS_MEMORY_COUNTERS counters{};
            counters.cb = sizeof(counters);
            if (GetProcessMemoryInfo(GetCurrentProcess(), &counters, sizeof(counters)) == 0)
            {
                return 0;
            }
            return counters.WorkingSetSize;
#elif defined(__APPLE__)
            rusage_info_current info{};
            if (proc_pid_rusage(getpid(), RUSAGE_INFO_CURRENT,
                                reinterpret_cast<rusage_info_t*>(&info)) != 0)
            {
                return 0;
            }
            // The physical footprint is what Activity Monitor's "Memory" column reports; fall back
            // to the resident set on a build whose rusage lacks the field.
            return info.ri_phys_footprint != 0 ? info.ri_phys_footprint : info.ri_resident_size;
#else
            // Linux exposes the current resident set as the second field of /proc/self/statm, in
            // pages; getrusage's ru_maxrss is the peak, not the current, so it is not used here.
            std::FILE* const statm = std::fopen("/proc/self/statm", "r");
            if (statm == nullptr)
            {
                return 0;
            }
            unsigned long sizePages = 0;
            unsigned long residentPages = 0;
            const int read = std::fscanf(statm, "%lu %lu", &sizePages, &residentPages);
            std::fclose(statm);
            if (read != 2)
            {
                return 0;
            }
            return static_cast<u64>(residentPages) * static_cast<u64>(sysconf(_SC_PAGESIZE));
#endif
        }

        /// @brief Reads the host's total physical RAM in bytes, 0 when unavailable.
        u64 SystemMemoryBytes()
        {
#if defined(_WIN32)
            MEMORYSTATUSEX status{};
            status.dwLength = sizeof(status);
            if (GlobalMemoryStatusEx(&status) == 0)
            {
                return 0;
            }
            return status.ullTotalPhys;
#elif defined(__APPLE__)
            u64 bytes = 0;
            usize size = sizeof(bytes);
            if (sysctlbyname("hw.memsize", &bytes, &size, nullptr, 0) != 0)
            {
                return 0;
            }
            return bytes;
#else
            const long pages = sysconf(_SC_PHYS_PAGES);
            const long pageSize = sysconf(_SC_PAGESIZE);
            if (pages <= 0 || pageSize <= 0)
            {
                return 0;
            }
            return static_cast<u64>(pages) * static_cast<u64>(pageSize);
#endif
        }
    }

    SystemStatsSampler::SystemStatsSampler()
    {
        m_Last.SystemMemoryBytes = SystemMemoryBytes();
    }

    const SystemStats& SystemStatsSampler::Sample()
    {
        const std::chrono::steady_clock::time_point now = std::chrono::steady_clock::now();
        const u64 cpuNanos = ProcessCpuNanos();

        if (m_HasPrevious)
        {
            const f64 wallNanos = static_cast<f64>(
                std::chrono::duration_cast<std::chrono::nanoseconds>(now - m_LastWall).count());
            // A monotone CPU counter never runs backwards, but guard the subtraction anyway so a
            // platform quirk yields 0 rather than an enormous unsigned wrap.
            const u64 cpuDelta = cpuNanos >= m_LastCpuNanos ? cpuNanos - m_LastCpuNanos : 0;
            m_Last.CpuPercent =
                wallNanos > 0.0 ? static_cast<f32>(100.0 * static_cast<f64>(cpuDelta) / wallNanos)
                                : 0.0f;
        }

        m_LastCpuNanos = cpuNanos;
        m_LastWall = now;
        m_HasPrevious = true;

        m_Last.ProcessMemoryBytes = ProcessMemoryBytes();
        return m_Last;
    }
}
