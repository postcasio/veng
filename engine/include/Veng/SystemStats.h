#pragma once

#include <Veng/Veng.h>

#include <chrono>

namespace Veng
{
    /// @brief A snapshot of process- and host-level resource usage, sampled once per frame.
    ///
    /// The process figures describe this process; the memory total describes the host. Every field
    /// is zero on a platform whose sampler is not implemented, so a consumer draws "n/a" rather
    /// than a wrong number. CPU time and memory are read together from one platform call where the
    /// platform offers one.
    struct SystemStats
    {
        /// @brief Process CPU usage over the last sample interval, in percent, top-style.
        ///
        /// 100 means one core fully busy; a multi-threaded process spanning cores exceeds 100 (e.g.
        /// 250 is two and a half cores' worth). Computed as consumed CPU time over wall time since
        /// the previous SystemStatsSampler::Sample(), so it is an interval average, not an instant.
        /// Zero on the first sample (no interval yet) and where CPU time is unavailable.
        f32 CpuPercent = 0.0f;

        /// @brief Resident/physical memory footprint of this process, in bytes.
        ///
        /// The macOS physical footprint (the figure Activity Monitor's "Memory" column shows) where
        /// available, else the resident set size. Zero where process memory is unavailable.
        u64 ProcessMemoryBytes = 0;

        /// @brief Total physical RAM installed on the host, in bytes.
        ///
        /// Constant for the process lifetime. Zero where the host memory total is unavailable.
        u64 SystemMemoryBytes = 0;
    };

    /// @brief Samples process CPU usage and memory, deriving CPU percent from the sample interval.
    ///
    /// A stateful sampler: CPU percent is consumed-CPU-time over wall-clock-time between two
    /// Sample() calls, so it must be sampled at a steady cadence — once per frame — for the figure
    /// to read as a rate. The engine owns one and samples it at the frame boundary; a consumer
    /// reads the published snapshot through Application::GetSystemStats() rather than sampling its
    /// own. Every reading is best-effort: an unimplemented or failing platform query leaves the
    /// corresponding field zero rather than failing.
    class SystemStatsSampler
    {
    public:
        /// @brief Constructs a sampler with no prior interval; caches the host memory total.
        SystemStatsSampler();

        /// @brief Samples the current usage and returns the snapshot, updating GetLast().
        ///
        /// CpuPercent covers the interval since the previous Sample() (zero on the first call).
        /// Call at a steady cadence — once per frame — so the interval is a frame and the percent
        /// reads as an instantaneous rate.
        /// @return The freshly sampled snapshot.
        const SystemStats& Sample();

        /// @brief Returns the most recently sampled snapshot without resampling.
        /// @return The last snapshot, zero-initialised before the first Sample().
        [[nodiscard]] const SystemStats& GetLast() const { return m_Last; }

    private:
        /// @brief The most recent snapshot.
        SystemStats m_Last;
        /// @brief Total consumed process CPU time at the previous sample, in nanoseconds.
        u64 m_LastCpuNanos = 0;
        /// @brief Wall clock at the previous sample, for the CPU-percent interval denominator.
        std::chrono::steady_clock::time_point m_LastWall;
        /// @brief False until the first Sample() establishes a prior point to difference against.
        bool m_HasPrevious = false;
    };
}
