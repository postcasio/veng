#pragma once

#include <Veng/Veng.h>
#include <Veng/Diagnostics/Profiler.h>

#include <atomic>

// Gui/GuiCounters.h — per-frame tallies of the GUI's repeated work, sampled as profiler counters.
//
// A document drive can solve its layout and shape its text many times over a frame, across every
// document on screen, so a per-call counter sample would read as a run of ones. The call sites bump
// a tally instead, and the application samples and clears it once per frame, so each series reads
// as a count per frame. Under VE_PROFILE=OFF every function here is empty and nothing is counted.

namespace Veng::Gui::Counters
{
#if defined(VE_PROFILE) && VE_PROFILE
    /// @brief Layout solves since the last sample that did not early-out on a clean document.
    inline std::atomic<u64> g_SolveTally{0};
    /// @brief Text runs shaped since the last sample, by layout measurement and by draw alike.
    inline std::atomic<u64> g_ShapedRunTally{0};
#endif

    /// @brief Counts one layout solve that re-applied style and re-ran layout.
    inline void CountSolve() noexcept
    {
#if defined(VE_PROFILE) && VE_PROFILE
        g_SolveTally.fetch_add(1, std::memory_order_relaxed);
#endif
    }

    /// @brief Counts one text run shaped through a font.
    inline void CountShapedRun() noexcept
    {
#if defined(VE_PROFILE) && VE_PROFILE
        g_ShapedRunTally.fetch_add(1, std::memory_order_relaxed);
#endif
    }

    /// @brief Samples the tallies as the `Gui/Solves` and `Gui/ShapedRuns` counters and clears them.
    ///
    /// Called once per frame, after the frame's documents have driven.
    inline void SampleFrame() noexcept
    {
#if defined(VE_PROFILE) && VE_PROFILE
        // Cleared whether or not a thread is recording, so the first sample after recording starts
        // reads one frame's tally rather than everything since launch.
        const u64 solves = g_SolveTally.exchange(0, std::memory_order_relaxed);
        const u64 shapedRuns = g_ShapedRunTally.exchange(0, std::memory_order_relaxed);
        VE_PROFILE_COUNTER("Gui/Solves", static_cast<f64>(solves));
        VE_PROFILE_COUNTER("Gui/ShapedRuns", static_cast<f64>(shapedRuns));
#endif
    }
}
