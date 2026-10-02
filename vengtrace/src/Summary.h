#pragma once

#include <Veng/Veng.h>

#include "TraceDecoder.h"

// Per-frame aggregation of a decoded capture: the numbers a performance question starts from, so
// they come from one reader rather than a throwaway script per measurement.
//
// Everything is measured over the capture's frame window: the main thread's frames less the first
// and the last. A triggered capture begins mid-frame, and its last frame has no successor to bound
// its period, so neither is whole; every frame between them is. GPU spans are back-dated to the
// frame that executed them, so a GPU frame belongs to the window by that index too.

namespace Veng::VengTrace
{
    /// @brief The tunables of a summary.
    struct SummaryOptions
    {
        /// @brief Rows in each ranked table: the steady-state scopes and each GPU track's passes.
        u32 Top = 15;
        /// @brief Frames reported as hitches, longest period first.
        u32 Hitches = 3;
        /// @brief Scopes listed under each hitch, by exclusive time.
        u32 HitchScopes = 8;
        /// @brief The smallest inclusive time, in milliseconds, a hitch's scope tree descends to.
        f64 TreeMinMs = 1.0;
        /// @brief Scope names read as idle beside the capture's own marks.
        ///
        /// For a capture written before scopes carried the mark (format version 1): a reader names
        /// the deliberate waits the capture cannot.
        vector<string> IdleScopes;
    };

    /// @brief Order statistics of a sample, in milliseconds unless stated otherwise.
    struct Distribution
    {
        /// @brief The number of values.
        usize Count = 0;
        /// @brief The smallest value.
        f64 Min = 0.0;
        /// @brief The 10th percentile, linearly interpolated.
        f64 P10 = 0.0;
        /// @brief The median (the mean of the middle two for an even count).
        f64 Median = 0.0;
        /// @brief The arithmetic mean.
        f64 Mean = 0.0;
        /// @brief The 90th percentile, linearly interpolated.
        f64 P90 = 0.0;
        /// @brief The 99th percentile, linearly interpolated.
        f64 P99 = 0.0;
        /// @brief The largest value.
        f64 Max = 0.0;
    };

    /// @brief Computes a distribution over a sample.
    /// @param values  The values; need not be sorted.
    /// @return The distribution; all zeros for an empty sample.
    [[nodiscard]] Distribution Distribute(vector<f64> values);

    /// @brief The frames a capture's statistics are taken over.
    struct FrameWindow
    {
        /// @brief The main thread's track id; 0 when the capture has no CPU scopes.
        u32 MainThread = 0;
        /// @brief The main thread's display name.
        string MainThreadName;
        /// @brief The whole frames, ascending: every main-thread frame but the first and the last.
        vector<u64> Frames;
        /// @brief Each window frame's start, in ticks: its first main-thread scope's begin.
        vector<u64> StartTicks;
        /// @brief Each window frame's period, in ticks: the next frame's start less its own.
        vector<u64> PeriodTicks;

        /// @brief Returns the index of a frame within the window, or nullopt when outside it.
        /// @param frame  The frame index.
        [[nodiscard]] optional<usize> IndexOf(u64 frame) const;
    };

    /// @brief Finds a capture's main thread and its frame window.
    ///
    /// The main thread is the CPU thread track named "Main" (the name the profiler registers its
    /// constructing thread under), or failing that the thread recording the most scopes.
    /// @param trace  The decoded capture.
    /// @return The window; empty when the main thread recorded fewer than three frames.
    [[nodiscard]] FrameWindow FindFrameWindow(const DecodedTrace& trace);

    /// @brief The frame-level figures.
    struct FrameSummary
    {
        /// @brief Start-to-start time of each window frame.
        Distribution Period;
        /// @brief The union of each frame's main-thread scopes, less its idle-marked scopes.
        Distribution Work;
        /// @brief The union of each frame's idle-marked main-thread scopes.
        Distribution Idle;
    };

    /// @brief One scope's per-frame exclusive cost on the main thread.
    struct ScopeCost
    {
        /// @brief The scope's name.
        string Name;
        /// @brief The median over window frames, counting a frame it did not run in as zero.
        f64 MedianMs = 0.0;
        /// @brief The mean over window frames.
        f64 MeanMs = 0.0;
        /// @brief The largest single frame's cost.
        f64 WorstMs = 0.0;
        /// @brief The frame the worst cost fell in.
        u64 WorstFrame = 0;
        /// @brief The window frames the scope ran in.
        usize Frames = 0;
    };

    /// @brief One scope instance in a hitch frame's tree.
    struct HitchNode
    {
        /// @brief The scope's name.
        string Name;
        /// @brief Nesting depth; 0 for a top-level scope.
        u32 Depth = 0;
        /// @brief The scope's duration, children included.
        f64 InclusiveMs = 0.0;
        /// @brief The scope's duration less its children's: its own, unscoped work.
        f64 ExclusiveMs = 0.0;
        /// @brief Whether the scope is marked idle.
        bool Idle = false;
    };

    /// @brief One scope's exclusive total within a single frame.
    struct NamedMs
    {
        /// @brief The scope's name.
        string Name;
        /// @brief The milliseconds.
        f64 Ms = 0.0;
    };

    /// @brief One of the capture's longest frames, broken down.
    struct Hitch
    {
        /// @brief The frame index.
        u64 Frame = 0;
        /// @brief The frame's period.
        f64 PeriodMs = 0.0;
        /// @brief The frame's work: its main-thread scopes' union less its idle scopes.
        f64 WorkMs = 0.0;
        /// @brief The frame's idle-marked time.
        f64 IdleMs = 0.0;
        /// @brief The period no main-thread scope covers.
        f64 UnscopedMs = 0.0;
        /// @brief The frame's largest exclusive costs, by scope name, idle scopes excluded.
        vector<NamedMs> TopExclusive;
        /// @brief The frame's main-thread scopes of at least SummaryOptions::TreeMinMs, in time order.
        vector<HitchNode> Tree;
    };

    /// @brief One value of a small-integer counter and how many samples took it.
    struct HistogramBin
    {
        /// @brief The value.
        i64 Value = 0;
        /// @brief The samples (or frames) that took it.
        usize Count = 0;
    };

    /// @brief One counter's samples over the window.
    struct CounterSummary
    {
        /// @brief The counter's name.
        string Name;
        /// @brief The samples' values.
        Distribution Samples;
        /// @brief The sum of each window frame's samples, a frame with none counting as zero.
        Distribution PerFrameSum;
        /// @brief Samples by value, when every sample is a small non-negative integer; else empty.
        vector<HistogramBin> Histogram;
    };

    /// @brief One GPU pass's per-frame cost.
    struct GpuPassCost
    {
        /// @brief The pass's name.
        string Name;
        /// @brief The GPU frames the pass ran in.
        usize Frames = 0;
        /// @brief The median over the frames it ran in.
        f64 MedianMs = 0.0;
        /// @brief The mean over the frames it ran in.
        f64 MeanMs = 0.0;
    };

    /// @brief Where a frame's largest untimed gap fell, and how often.
    struct GpuGapTally
    {
        /// @brief The pass that ends where the gap begins, or "start" for the frame's start.
        string Before;
        /// @brief The pass that begins where the gap ends, or "end" for the frame's end.
        string After;
        /// @brief The GPU frames whose largest gap fell here.
        usize Frames = 0;
    };

    /// @brief One GPU track's per-frame figures.
    struct GpuSummary
    {
        /// @brief The track's display name.
        string Track;
        /// @brief Each GPU frame's span: the span enclosing every other in the frame, if one does,
        /// else the passes' extent.
        Distribution Frame;
        /// @brief The union of each frame's passes.
        Distribution PassUnion;
        /// @brief Each frame less the union of its passes: GPU time no pass accounts for.
        Distribution Untimed;
        /// @brief The SummaryOptions::Top passes by median, descending.
        vector<GpuPassCost> Passes;
        /// @brief Where the largest gap fell, most frequent first.
        vector<GpuGapTally> LargestGaps;
    };

    /// @brief One world's fixed-step simulation, read from the step counter its sim scope samples.
    struct SimWorldSummary
    {
        /// @brief The scope the world's step counter is sampled inside (the world's sim scope).
        string Scope;
        /// @brief Window frames by steps taken.
        vector<HistogramBin> StepsPerFrame;
        /// @brief Steps taken over the window.
        u64 Steps = 0;
        /// @brief The scope's time over the window divided by the steps taken.
        f64 MeanMsPerStep = 0.0;
        /// @brief The median over frames that stepped of the scope's time over that frame's steps.
        f64 MedianMsPerStep = 0.0;
    };

    /// @brief Everything `vengtrace summary` reports.
    struct CaptureSummary
    {
        /// @brief The capture's format version.
        u32 FormatVersion = 0;
        /// @brief Whether the capture was cut short.
        bool Truncated = false;
        /// @brief Events the producer dropped.
        u64 DroppedEvents = 0;
        /// @brief The executable that produced the capture, if recorded.
        string Executable;
        /// @brief The main thread's display name.
        string MainThread;
        /// @brief The first and last window frames; equal and zero when the window is empty.
        u64 FirstFrame = 0;
        /// @brief The last window frame.
        u64 LastFrame = 0;
        /// @brief Whether any scope in the capture is idle-marked by the capture itself.
        bool HasIdleMarks = false;
        /// @brief The scope names SummaryOptions::IdleScopes added to the capture's own marks.
        vector<string> ExtraIdleScopes;
        /// @brief The frame-level figures.
        FrameSummary Frames;
        /// @brief The main thread's scopes by median per-frame exclusive cost, idle scopes excluded.
        vector<ScopeCost> SteadyState;
        /// @brief The longest frames.
        vector<Hitch> Hitches;
        /// @brief The smallest inclusive time, in milliseconds, each hitch's tree lists.
        f64 TreeMinMs = 0.0;
        /// @brief Every counter, by name.
        vector<CounterSummary> Counters;
        /// @brief One entry per GPU track.
        vector<GpuSummary> Gpu;
        /// @brief One entry per world whose sim scope samples the step counter.
        vector<SimWorldSummary> Sim;
    };

    /// @brief Aggregates a decoded capture per frame.
    /// @param trace    The decoded capture.
    /// @param options  The table sizes and the hitch tree's cutoff.
    /// @return The summary.
    [[nodiscard]] CaptureSummary Summarize(const DecodedTrace& trace,
                                           const SummaryOptions& options);

    /// @brief One scope present in both compared captures.
    struct ScopeComparison
    {
        /// @brief The scope's name.
        string Name;
        /// @brief The virtual track the scope rode (a GPU pass's "GPU"), or empty for a CPU thread's.
        string Lane;
        /// @brief Calls in the first capture's window.
        usize CallsA = 0;
        /// @brief Calls in the second capture's window.
        usize CallsB = 0;
        /// @brief The per-call median in the first capture.
        f64 MedianMsA = 0.0;
        /// @brief The per-call median in the second capture.
        f64 MedianMsB = 0.0;
        /// @brief MedianMsA over MedianMsB; nullopt when the second median is zero.
        optional<f64> Ratio;
    };

    /// @brief The spread of one lane's per-scope ratios.
    struct LaneRatio
    {
        /// @brief The lane: empty for the CPU threads, else the virtual track's name.
        string Lane;
        /// @brief The ratios of the lane's scopes whose medians are at least RatioFloorMs in both.
        Distribution Ratio;
    };

    /// @brief The per-call comparison of two captures.
    struct Comparison
    {
        /// @brief Every scope common to both, by the larger of its two medians, descending.
        vector<ScopeComparison> Scopes;
        /// @brief The spread of the ratios, per lane, the CPU threads first.
        ///
        /// A uniform ratio across unrelated scopes is the signature of a slowed machine rather
        /// than of a code change. Lanes are kept apart because a hot machine need not slow its
        /// CPU and GPU alike; the floor keeps the trace clock's tick from quantizing a ratio.
        vector<LaneRatio> Ratios;
        /// @brief The per-call median a scope needs in both captures to count toward Ratios.
        f64 RatioFloorMs = 0.01;
        /// @brief Scopes (by name and lane) only the first capture recorded.
        usize OnlyInA = 0;
        /// @brief Scopes (by name and lane) only the second capture recorded.
        usize OnlyInB = 0;
    };

    /// @brief Compares the per-call medians of every scope two captures share, over each one's window.
    ///
    /// A scope is its name on its lane: a CPU thread's scopes pool across threads by name, and a
    /// virtual track's (the GPU's passes) stay apart from a CPU scope of the same name.
    /// @param a  The first capture.
    /// @param b  The second capture.
    /// @return The comparison; ratios are a over b.
    [[nodiscard]] Comparison Compare(const DecodedTrace& a, const DecodedTrace& b);
}
