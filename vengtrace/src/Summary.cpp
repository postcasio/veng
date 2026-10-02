#include "Summary.h"

#include <algorithm>
#include <cmath>
#include <map>

namespace Veng::VengTrace
{
    namespace
    {
        // The engine's fixed-step counter: the world runner samples it once per world per frame,
        // inside that world's sim scope, with the steps the world's clock ran.
        constexpr string_view SimStepsCounter = "WorldRunner/SimSteps";

        // A counter whose every sample is a non-negative integer no larger than this, over no more
        // than SmallIntegerMaxDistinct distinct values, is reported as a histogram as well.
        constexpr f64 SmallIntegerMax = 32.0;
        constexpr usize SmallIntegerMaxDistinct = 16;

        constexpr string_view GapStart = "start";
        constexpr string_view GapEnd = "end";

        struct Interval
        {
            u64 Begin = 0;
            u64 End = 0;
        };

        // The length of the union of a set of intervals.
        u64 UnionTicks(vector<Interval> intervals)
        {
            std::ranges::sort(intervals, [](const Interval& left, const Interval& right)
                              { return left.Begin < right.Begin; });
            u64 total = 0;
            optional<Interval> run;
            for (const Interval& interval : intervals)
            {
                if (run && interval.Begin <= run->End)
                {
                    run->End = std::max(run->End, interval.End);
                    continue;
                }
                if (run)
                {
                    total += run->End - run->Begin;
                }
                run = interval;
            }
            if (run)
            {
                total += run->End - run->Begin;
            }
            return total;
        }

        f64 Quantile(const vector<f64>& sorted, f64 fraction)
        {
            const f64 rank = fraction * static_cast<f64>(sorted.size() - 1);
            const auto low = static_cast<usize>(std::floor(rank));
            const auto high = static_cast<usize>(std::ceil(rank));
            return sorted[low] + (sorted[high] - sorted[low]) * (rank - static_cast<f64>(low));
        }

        // One main-thread scope, placed in the thread's nesting.
        struct Node
        {
            usize Event = 0;
            u32 Depth = 0;
            u64 ChildTicks = 0;
        };

        // The main thread's scopes in the window, grouped by window frame, each in begin order.
        struct MainThreadScopes
        {
            vector<Node> Nodes;
            vector<vector<usize>> ByFrame;
        };

        // Recovers the main thread's nesting from containment — scoped timing nests by construction
        // — and groups the window's scopes by frame.
        MainThreadScopes BuildMainThreadScopes(const DecodedTrace& trace, const FrameWindow& window)
        {
            vector<usize> order;
            for (usize index = 0; index < trace.Events.size(); ++index)
            {
                const Event& event = trace.Events[index];
                if (event.Type == RecordType::ScopeComplete && !event.HasVirtualTrack &&
                    event.Thread == window.MainThread)
                {
                    order.push_back(index);
                }
            }
            // Begin order, an enclosing scope before what it encloses. A scope commits after its
            // children, so of two identical spans the later record is the parent.
            std::ranges::sort(order,
                              [&trace](usize left, usize right)
                              {
                                  const Event& a = trace.Events[left];
                                  const Event& b = trace.Events[right];
                                  if (a.BeginTicks != b.BeginTicks)
                                  {
                                      return a.BeginTicks < b.BeginTicks;
                                  }
                                  if (a.EndTicks != b.EndTicks)
                                  {
                                      return a.EndTicks > b.EndTicks;
                                  }
                                  return left > right;
                              });

            MainThreadScopes scopes;
            scopes.ByFrame.resize(window.Frames.size());
            vector<usize> stack;
            for (const usize index : order)
            {
                const Event& event = trace.Events[index];
                while (!stack.empty())
                {
                    const Event& top = trace.Events[scopes.Nodes[stack.back()].Event];
                    if (top.EndTicks >= event.EndTicks && top.EndTicks > event.BeginTicks)
                    {
                        break;
                    }
                    stack.pop_back();
                }
                const Node node{.Event = index, .Depth = static_cast<u32>(stack.size())};
                if (!stack.empty())
                {
                    scopes.Nodes[stack.back()].ChildTicks += event.EndTicks - event.BeginTicks;
                }
                stack.push_back(scopes.Nodes.size());
                scopes.Nodes.push_back(node);
                if (const optional<usize> frame = window.IndexOf(event.Frame))
                {
                    scopes.ByFrame[*frame].push_back(scopes.Nodes.size() - 1);
                }
            }
            return scopes;
        }

        u64 ExclusiveTicks(const DecodedTrace& trace, const Node& node)
        {
            const Event& event = trace.Events[node.Event];
            const u64 inclusive = event.EndTicks - event.BeginTicks;
            return inclusive > node.ChildTicks ? inclusive - node.ChildTicks : 0;
        }

        // A GPU frame's interval and its passes: the span enclosing every other, if one does, else
        // the passes' extent.
        struct GpuFrame
        {
            Interval Span;
            vector<usize> Passes;
        };

        GpuFrame ResolveGpuFrame(const DecodedTrace& trace, const vector<usize>& spans)
        {
            usize longest = spans.front();
            for (const usize index : spans)
            {
                const Event& event = trace.Events[index];
                const Event& best = trace.Events[longest];
                if (event.EndTicks - event.BeginTicks > best.EndTicks - best.BeginTicks)
                {
                    longest = index;
                }
            }
            const Event& outer = trace.Events[longest];
            // A frame span and its passes are placed from separately rounded readings, so a pass may
            // end a little past the span that encloses it.
            const u64 slack = (outer.EndTicks - outer.BeginTicks) / 64;
            bool encloses = spans.size() > 1;
            Interval extent{.Begin = outer.BeginTicks, .End = outer.EndTicks};
            for (const usize index : spans)
            {
                const Event& event = trace.Events[index];
                encloses = encloses && event.BeginTicks >= outer.BeginTicks &&
                           event.EndTicks <= outer.EndTicks + slack;
                extent.Begin = std::min(extent.Begin, event.BeginTicks);
                extent.End = std::max(extent.End, event.EndTicks);
            }

            GpuFrame frame;
            if (encloses)
            {
                frame.Span = Interval{.Begin = outer.BeginTicks, .End = outer.EndTicks};
                for (const usize index : spans)
                {
                    if (index != longest)
                    {
                        frame.Passes.push_back(index);
                    }
                }
            }
            else
            {
                frame.Span = extent;
                frame.Passes = spans;
            }
            return frame;
        }

        // One run of overlapping passes: its extent, the pass that opens it and the pass that closes
        // it. An enclosing pass sorts ahead of what it encloses, so on a tie the outermost names it.
        struct PassRun
        {
            Interval Extent;
            usize First = 0;
            usize Last = 0;
        };

        vector<PassRun> MergePasses(const DecodedTrace& trace, vector<usize> passes,
                                    const Interval& frame)
        {
            std::ranges::sort(passes,
                              [&trace](usize left, usize right)
                              {
                                  const Event& a = trace.Events[left];
                                  const Event& b = trace.Events[right];
                                  if (a.BeginTicks != b.BeginTicks)
                                  {
                                      return a.BeginTicks < b.BeginTicks;
                                  }
                                  return a.EndTicks > b.EndTicks;
                              });
            vector<PassRun> runs;
            for (const usize index : passes)
            {
                const Event& event = trace.Events[index];
                // A pass overhanging the frame span (see ResolveGpuFrame) counts only to the frame's end.
                const Interval clamped{.Begin =
                                           std::clamp(event.BeginTicks, frame.Begin, frame.End),
                                       .End = std::clamp(event.EndTicks, frame.Begin, frame.End)};
                if (!runs.empty() && clamped.Begin <= runs.back().Extent.End)
                {
                    if (clamped.End > runs.back().Extent.End)
                    {
                        runs.back().Extent.End = clamped.End;
                        runs.back().Last = index;
                    }
                    continue;
                }
                runs.push_back(PassRun{.Extent = clamped, .First = index, .Last = index});
            }
            return runs;
        }

        vector<HistogramBin> ToHistogram(const std::map<i64, usize>& bins)
        {
            vector<HistogramBin> histogram;
            for (const auto& [value, count] : bins)
            {
                histogram.push_back(HistogramBin{.Value = value, .Count = count});
            }
            return histogram;
        }

        struct Summarizer
        {
            const DecodedTrace& Trace;
            const SummaryOptions& Options;
            FrameWindow Window;
            MainThreadScopes Scopes;
            f64 MsPerTick = 0.0;

            [[nodiscard]] f64 Ms(u64 ticks) const { return static_cast<f64>(ticks) * MsPerTick; }

            [[nodiscard]] string Name(u32 id) const { return string(Trace.Resolve(id)); }

            void SummarizeFrames(CaptureSummary& summary, vector<f64>& workMs,
                                 vector<f64>& idleMs) const
            {
                vector<f64> periodMs;
                for (usize frame = 0; frame < Window.Frames.size(); ++frame)
                {
                    vector<Interval> all;
                    vector<Interval> idle;
                    for (const usize node : Scopes.ByFrame[frame])
                    {
                        const Event& event = Trace.Events[Scopes.Nodes[node].Event];
                        const Interval span{.Begin = event.BeginTicks, .End = event.EndTicks};
                        all.push_back(span);
                        if (event.Idle)
                        {
                            idle.push_back(span);
                        }
                    }
                    const u64 idleTicks = UnionTicks(idle);
                    const u64 allTicks = UnionTicks(all);
                    periodMs.push_back(Ms(Window.PeriodTicks[frame]));
                    workMs.push_back(Ms(allTicks - std::min(allTicks, idleTicks)));
                    idleMs.push_back(Ms(idleTicks));
                }
                summary.Frames.Period = Distribute(periodMs);
                summary.Frames.Work = Distribute(workMs);
                summary.Frames.Idle = Distribute(idleMs);
            }

            void SummarizeSteadyState(CaptureSummary& summary) const
            {
                struct PerFrame
                {
                    vector<f64> Costs;
                    vector<bool> Ran;
                };
                std::map<string, PerFrame> scopes;
                for (usize frame = 0; frame < Window.Frames.size(); ++frame)
                {
                    for (const usize node : Scopes.ByFrame[frame])
                    {
                        const Event& event = Trace.Events[Scopes.Nodes[node].Event];
                        if (event.Idle)
                        {
                            continue;
                        }
                        PerFrame& scope = scopes[Name(event.Name)];
                        scope.Costs.resize(Window.Frames.size(), 0.0);
                        scope.Ran.resize(Window.Frames.size(), false);
                        scope.Costs[frame] += Ms(ExclusiveTicks(Trace, Scopes.Nodes[node]));
                        scope.Ran[frame] = true;
                    }
                }
                for (auto& [name, scope] : scopes)
                {
                    ScopeCost cost{.Name = name};
                    for (usize frame = 0; frame < scope.Costs.size(); ++frame)
                    {
                        cost.Frames += scope.Ran[frame] ? 1 : 0;
                        if (scope.Costs[frame] > cost.WorstMs)
                        {
                            cost.WorstMs = scope.Costs[frame];
                            cost.WorstFrame = Window.Frames[frame];
                        }
                    }
                    const Distribution distribution = Distribute(std::move(scope.Costs));
                    cost.MedianMs = distribution.Median;
                    cost.MeanMs = distribution.Mean;
                    summary.SteadyState.push_back(std::move(cost));
                }
                std::ranges::sort(summary.SteadyState,
                                  [](const ScopeCost& left, const ScopeCost& right)
                                  {
                                      if (left.MedianMs != right.MedianMs)
                                      {
                                          return left.MedianMs > right.MedianMs;
                                      }
                                      if (left.MeanMs != right.MeanMs)
                                      {
                                          return left.MeanMs > right.MeanMs;
                                      }
                                      return left.Name < right.Name;
                                  });
                if (summary.SteadyState.size() > Options.Top)
                {
                    summary.SteadyState.resize(Options.Top);
                }
            }

            void SummarizeHitches(CaptureSummary& summary, const vector<f64>& workMs,
                                  const vector<f64>& idleMs) const
            {
                vector<usize> frames(Window.Frames.size());
                for (usize frame = 0; frame < frames.size(); ++frame)
                {
                    frames[frame] = frame;
                }
                std::ranges::stable_sort(
                    frames, [this](usize left, usize right)
                    { return Window.PeriodTicks[left] > Window.PeriodTicks[right]; });
                frames.resize(std::min<usize>(frames.size(), Options.Hitches));

                for (const usize frame : frames)
                {
                    Hitch hitch{.Frame = Window.Frames[frame],
                                .PeriodMs = Ms(Window.PeriodTicks[frame]),
                                .WorkMs = workMs[frame],
                                .IdleMs = idleMs[frame]};
                    hitch.UnscopedMs = std::max(0.0, hitch.PeriodMs - hitch.WorkMs - hitch.IdleMs);

                    std::map<string, f64> exclusive;
                    for (const usize index : Scopes.ByFrame[frame])
                    {
                        const Node& node = Scopes.Nodes[index];
                        const Event& event = Trace.Events[node.Event];
                        const f64 exclusiveMs = Ms(ExclusiveTicks(Trace, node));
                        if (!event.Idle)
                        {
                            exclusive[Name(event.Name)] += exclusiveMs;
                        }
                        const f64 inclusiveMs = Ms(event.EndTicks - event.BeginTicks);
                        if (inclusiveMs >= Options.TreeMinMs)
                        {
                            hitch.Tree.push_back(HitchNode{.Name = Name(event.Name),
                                                           .Depth = node.Depth,
                                                           .InclusiveMs = inclusiveMs,
                                                           .ExclusiveMs = exclusiveMs,
                                                           .Idle = event.Idle});
                        }
                    }
                    for (const auto& [name, ms] : exclusive)
                    {
                        hitch.TopExclusive.push_back(NamedMs{.Name = name, .Ms = ms});
                    }
                    std::ranges::stable_sort(hitch.TopExclusive,
                                             [](const NamedMs& left, const NamedMs& right)
                                             { return left.Ms > right.Ms; });
                    if (hitch.TopExclusive.size() > Options.HitchScopes)
                    {
                        hitch.TopExclusive.resize(Options.HitchScopes);
                    }
                    summary.Hitches.push_back(std::move(hitch));
                }
            }

            void SummarizeCounters(CaptureSummary& summary) const
            {
                struct Samples
                {
                    vector<f64> Values;
                    vector<f64> PerFrame;
                };
                std::map<string, Samples> counters;
                for (const Event& event : Trace.Events)
                {
                    if (event.Type != RecordType::Counter)
                    {
                        continue;
                    }
                    const optional<usize> frame = Window.IndexOf(event.Frame);
                    if (!frame)
                    {
                        continue;
                    }
                    Samples& samples = counters[Name(event.Name)];
                    samples.PerFrame.resize(Window.Frames.size(), 0.0);
                    samples.Values.push_back(event.Value);
                    samples.PerFrame[*frame] += event.Value;
                }
                for (auto& [name, samples] : counters)
                {
                    CounterSummary counter{.Name = name};
                    std::map<i64, usize> bins;
                    bool small = true;
                    for (const f64 value : samples.Values)
                    {
                        small = small && value >= 0.0 && value <= SmallIntegerMax &&
                                value == std::floor(value);
                        if (small)
                        {
                            ++bins[static_cast<i64>(value)];
                        }
                    }
                    if (small && bins.size() <= SmallIntegerMaxDistinct)
                    {
                        counter.Histogram = ToHistogram(bins);
                    }
                    counter.Samples = Distribute(std::move(samples.Values));
                    counter.PerFrameSum = Distribute(std::move(samples.PerFrame));
                    summary.Counters.push_back(std::move(counter));
                }
            }

            void SummarizeGpuTrack(CaptureSummary& summary, const Track& track) const
            {
                vector<vector<usize>> byFrame(Window.Frames.size());
                for (usize index = 0; index < Trace.Events.size(); ++index)
                {
                    const Event& event = Trace.Events[index];
                    if (event.Type != RecordType::ScopeComplete || !event.HasVirtualTrack ||
                        event.VirtualTrack != track.Id)
                    {
                        continue;
                    }
                    if (const optional<usize> frame = Window.IndexOf(event.Frame))
                    {
                        byFrame[*frame].push_back(index);
                    }
                }

                GpuSummary gpu{.Track = track.Name};
                vector<f64> frameMs;
                vector<f64> unionMs;
                vector<f64> untimedMs;
                std::map<string, vector<f64>> passMs;
                std::map<std::pair<string, string>, usize> gaps;
                for (const vector<usize>& spans : byFrame)
                {
                    if (spans.empty())
                    {
                        continue;
                    }
                    const GpuFrame frame = ResolveGpuFrame(Trace, spans);
                    std::map<string, f64> passes;
                    for (const usize index : frame.Passes)
                    {
                        const Event& event = Trace.Events[index];
                        passes[Name(event.Name)] += Ms(event.EndTicks - event.BeginTicks);
                    }
                    for (const auto& [name, ms] : passes)
                    {
                        passMs[name].push_back(ms);
                    }

                    const vector<PassRun> runs = MergePasses(Trace, frame.Passes, frame.Span);
                    u64 covered = 0;
                    for (const PassRun& run : runs)
                    {
                        covered += run.Extent.End - run.Extent.Begin;
                    }
                    const u64 frameTicks = frame.Span.End - frame.Span.Begin;
                    frameMs.push_back(Ms(frameTicks));
                    unionMs.push_back(Ms(covered));
                    untimedMs.push_back(Ms(frameTicks - std::min(frameTicks, covered)));

                    // The largest gap, the frame's start and end counting as passes either side.
                    string before(GapStart);
                    string after(GapEnd);
                    u64 largest = frameTicks;
                    if (!runs.empty())
                    {
                        largest = runs.front().Extent.Begin - frame.Span.Begin;
                        after = Name(Trace.Events[runs.front().First].Name);
                        for (usize run = 0; run + 1 < runs.size(); ++run)
                        {
                            const u64 gap = runs[run + 1].Extent.Begin - runs[run].Extent.End;
                            if (gap > largest)
                            {
                                largest = gap;
                                before = Name(Trace.Events[runs[run].Last].Name);
                                after = Name(Trace.Events[runs[run + 1].First].Name);
                            }
                        }
                        if (frame.Span.End - runs.back().Extent.End > largest)
                        {
                            before = Name(Trace.Events[runs.back().Last].Name);
                            after = string(GapEnd);
                        }
                    }
                    ++gaps[{before, after}];
                }
                if (frameMs.empty())
                {
                    return;
                }

                gpu.Frame = Distribute(std::move(frameMs));
                gpu.PassUnion = Distribute(std::move(unionMs));
                gpu.Untimed = Distribute(std::move(untimedMs));
                for (auto& [name, ms] : passMs)
                {
                    const Distribution distribution = Distribute(std::move(ms));
                    gpu.Passes.push_back(GpuPassCost{.Name = name,
                                                     .Frames = distribution.Count,
                                                     .MedianMs = distribution.Median,
                                                     .MeanMs = distribution.Mean});
                }
                std::ranges::stable_sort(gpu.Passes,
                                         [](const GpuPassCost& left, const GpuPassCost& right)
                                         { return left.MedianMs > right.MedianMs; });
                if (gpu.Passes.size() > Options.Top)
                {
                    gpu.Passes.resize(Options.Top);
                }
                for (const auto& [pair, frames] : gaps)
                {
                    gpu.LargestGaps.push_back(
                        GpuGapTally{.Before = pair.first, .After = pair.second, .Frames = frames});
                }
                std::ranges::stable_sort(gpu.LargestGaps,
                                         [](const GpuGapTally& left, const GpuGapTally& right)
                                         { return left.Frames > right.Frames; });
                summary.Gpu.push_back(std::move(gpu));
            }

            void SummarizeSim(CaptureSummary& summary) const
            {
                struct World
                {
                    // Per window frame the world stepped in: its steps, and its sim scope's time.
                    std::map<usize, u64> Steps;
                    std::map<usize, u64> Ticks;
                };
                std::map<string, World> worlds;
                for (const Event& event : Trace.Events)
                {
                    if (event.Type != RecordType::Counter || event.HasVirtualTrack ||
                        event.Thread != Window.MainThread ||
                        Trace.Resolve(event.Name) != SimStepsCounter)
                    {
                        continue;
                    }
                    const optional<usize> frame = Window.IndexOf(event.Frame);
                    if (!frame)
                    {
                        continue;
                    }
                    // The world is the innermost scope the sample was taken inside.
                    optional<usize> innermost;
                    for (const usize node : Scopes.ByFrame[*frame])
                    {
                        const Event& scope = Trace.Events[Scopes.Nodes[node].Event];
                        if (scope.BeginTicks <= event.BeginTicks &&
                            event.BeginTicks <= scope.EndTicks &&
                            (!innermost ||
                             Scopes.Nodes[node].Depth > Scopes.Nodes[*innermost].Depth))
                        {
                            innermost = node;
                        }
                    }
                    if (!innermost)
                    {
                        continue;
                    }
                    const Event& scope = Trace.Events[Scopes.Nodes[*innermost].Event];
                    World& world = worlds[Name(scope.Name)];
                    world.Steps[*frame] += static_cast<u64>(std::max(0.0, event.Value));
                    world.Ticks[*frame] += scope.EndTicks - scope.BeginTicks;
                }

                for (const auto& [scope, world] : worlds)
                {
                    SimWorldSummary sim{.Scope = scope};
                    std::map<i64, usize> bins;
                    u64 ticks = 0;
                    vector<f64> perStep;
                    for (const auto& [frame, steps] : world.Steps)
                    {
                        ++bins[static_cast<i64>(steps)];
                        sim.Steps += steps;
                        const u64 frameTicks = world.Ticks.at(frame);
                        ticks += frameTicks;
                        if (steps > 0)
                        {
                            perStep.push_back(Ms(frameTicks) / static_cast<f64>(steps));
                        }
                    }
                    sim.StepsPerFrame = ToHistogram(bins);
                    sim.MeanMsPerStep =
                        sim.Steps > 0 ? Ms(ticks) / static_cast<f64>(sim.Steps) : 0.0;
                    sim.MedianMsPerStep = Distribute(std::move(perStep)).Median;
                    summary.Sim.push_back(std::move(sim));
                }
            }
        };

        // A scope's identity across two captures: its name and the virtual track it rode, if any. A
        // CPU pass's recording and the GPU pass it records share a name, and are different costs.
        using ScopeKey = std::pair<string, string>;

        // Every scope's per-call durations over a capture's window, every track included.
        std::map<ScopeKey, vector<f64>> PerCallDurations(const DecodedTrace& trace)
        {
            const FrameWindow window = FindFrameWindow(trace);
            const f64 msPerTick =
                trace.TickFrequency != 0 ? 1000.0 / static_cast<f64>(trace.TickFrequency) : 0.0;
            std::map<u32, string> virtualNames;
            for (const Track& track : trace.Tracks)
            {
                if (track.Kind == TrackKind::Virtual)
                {
                    virtualNames[track.Id] =
                        track.Name.empty() ? "Track " + std::to_string(track.Id) : track.Name;
                }
            }
            std::map<ScopeKey, vector<f64>> durations;
            for (const Event& event : trace.Events)
            {
                if (event.Type != RecordType::ScopeComplete || !window.IndexOf(event.Frame))
                {
                    continue;
                }
                string lane;
                if (event.HasVirtualTrack)
                {
                    const auto found = virtualNames.find(event.VirtualTrack);
                    lane = found != virtualNames.end()
                               ? found->second
                               : "Track " + std::to_string(event.VirtualTrack);
                }
                durations[{string(trace.Resolve(event.Name)), lane}].push_back(
                    static_cast<f64>(event.EndTicks - event.BeginTicks) * msPerTick);
            }
            return durations;
        }
    }

    Distribution Distribute(vector<f64> values)
    {
        Distribution distribution;
        if (values.empty())
        {
            return distribution;
        }
        std::ranges::sort(values);
        f64 sum = 0.0;
        for (const f64 value : values)
        {
            sum += value;
        }
        distribution.Count = values.size();
        distribution.Min = values.front();
        distribution.Max = values.back();
        distribution.Mean = sum / static_cast<f64>(values.size());
        distribution.P10 = Quantile(values, 0.1);
        distribution.Median = Quantile(values, 0.5);
        distribution.P90 = Quantile(values, 0.9);
        distribution.P99 = Quantile(values, 0.99);
        return distribution;
    }

    optional<usize> FrameWindow::IndexOf(u64 frame) const
    {
        const auto found = std::ranges::lower_bound(Frames, frame);
        if (found == Frames.end() || *found != frame)
        {
            return std::nullopt;
        }
        return static_cast<usize>(found - Frames.begin());
    }

    FrameWindow FindFrameWindow(const DecodedTrace& trace)
    {
        FrameWindow window;
        for (const Track& track : trace.Tracks)
        {
            if (track.Kind == TrackKind::Thread && track.Role == TrackRole::Cpu &&
                track.Name == "Main")
            {
                window.MainThread = track.Id;
                window.MainThreadName = track.Name;
                break;
            }
        }
        if (window.MainThread == 0)
        {
            std::map<u32, usize> scopesByThread;
            for (const Event& event : trace.Events)
            {
                if (event.Type == RecordType::ScopeComplete && !event.HasVirtualTrack)
                {
                    ++scopesByThread[event.Thread];
                }
            }
            usize most = 0;
            for (const auto& [thread, count] : scopesByThread)
            {
                if (count > most)
                {
                    most = count;
                    window.MainThread = thread;
                }
            }
            window.MainThreadName = "Thread " + std::to_string(window.MainThread);
        }

        // Each main-thread frame starts at its first scope's begin.
        std::map<u64, u64> starts;
        for (const Event& event : trace.Events)
        {
            if (event.Type == RecordType::ScopeComplete && !event.HasVirtualTrack &&
                event.Thread == window.MainThread)
            {
                const auto [found, inserted] = starts.try_emplace(event.Frame, event.BeginTicks);
                if (!inserted)
                {
                    found->second = std::min(found->second, event.BeginTicks);
                }
            }
        }
        if (starts.size() < 3)
        {
            return window;
        }

        // The first frame was entered before the capture began and the last has no successor to
        // bound its period, so the window is everything between them.
        const vector<std::pair<u64, u64>> ordered(starts.begin(), starts.end());
        for (usize index = 1; index + 1 < ordered.size(); ++index)
        {
            window.Frames.push_back(ordered[index].first);
            window.StartTicks.push_back(ordered[index].second);
            const u64 next = ordered[index + 1].second;
            window.PeriodTicks.push_back(next > ordered[index].second ? next - ordered[index].second
                                                                      : 0);
        }
        return window;
    }

    CaptureSummary Summarize(const DecodedTrace& trace, const SummaryOptions& options)
    {
        CaptureSummary summary;
        summary.FormatVersion = trace.FormatVersion;
        summary.Truncated = trace.Truncated;
        summary.DroppedEvents = trace.DroppedEvents;
        if (trace.Meta)
        {
            summary.Executable = trace.Meta->ExecutableBasename;
        }
        for (const Event& event : trace.Events)
        {
            summary.HasIdleMarks = summary.HasIdleMarks || event.Idle;
        }
        summary.ExtraIdleScopes = options.IdleScopes;
        summary.TreeMinMs = options.TreeMinMs;

        // Names given as idle are applied to a copy, so every figure below reads one mark.
        optional<DecodedTrace> marked;
        if (!options.IdleScopes.empty())
        {
            marked = trace;
            for (Event& event : marked->Events)
            {
                event.Idle = event.Idle ||
                             (event.Type == RecordType::ScopeComplete &&
                              std::ranges::find(options.IdleScopes, marked->Resolve(event.Name)) !=
                                  options.IdleScopes.end());
            }
        }
        const DecodedTrace& source = marked ? *marked : trace;

        Summarizer summarizer{.Trace = source, .Options = options};
        summarizer.Window = FindFrameWindow(source);
        summarizer.MsPerTick =
            trace.TickFrequency != 0 ? 1000.0 / static_cast<f64>(trace.TickFrequency) : 0.0;
        summary.MainThread = summarizer.Window.MainThreadName;
        if (summarizer.Window.Frames.empty())
        {
            return summary;
        }
        summary.FirstFrame = summarizer.Window.Frames.front();
        summary.LastFrame = summarizer.Window.Frames.back();
        summarizer.Scopes = BuildMainThreadScopes(source, summarizer.Window);

        vector<f64> workMs;
        vector<f64> idleMs;
        summarizer.SummarizeFrames(summary, workMs, idleMs);
        summarizer.SummarizeSteadyState(summary);
        summarizer.SummarizeHitches(summary, workMs, idleMs);
        summarizer.SummarizeCounters(summary);
        for (const Track& track : source.Tracks)
        {
            if (track.Kind == TrackKind::Virtual && track.Role == TrackRole::Gpu)
            {
                summarizer.SummarizeGpuTrack(summary, track);
            }
        }
        summarizer.SummarizeSim(summary);
        return summary;
    }

    Comparison Compare(const DecodedTrace& a, const DecodedTrace& b)
    {
        Comparison comparison;
        const std::map<ScopeKey, vector<f64>> left = PerCallDurations(a);
        const std::map<ScopeKey, vector<f64>> right = PerCallDurations(b);
        std::map<string, vector<f64>> ratios;
        for (const auto& [key, durations] : left)
        {
            const auto other = right.find(key);
            if (other == right.end())
            {
                ++comparison.OnlyInA;
                continue;
            }
            ScopeComparison row{.Name = key.first,
                                .Lane = key.second,
                                .CallsA = durations.size(),
                                .CallsB = other->second.size(),
                                .MedianMsA = Distribute(durations).Median,
                                .MedianMsB = Distribute(other->second).Median};
            if (row.MedianMsB > 0.0)
            {
                row.Ratio = row.MedianMsA / row.MedianMsB;
                if (row.MedianMsA >= comparison.RatioFloorMs &&
                    row.MedianMsB >= comparison.RatioFloorMs)
                {
                    ratios[key.second].push_back(*row.Ratio);
                }
            }
            comparison.Scopes.push_back(std::move(row));
        }
        for (const auto& [key, durations] : right)
        {
            comparison.OnlyInB += left.contains(key) ? 0 : 1;
        }
        for (auto& [lane, values] : ratios)
        {
            comparison.Ratios.push_back(
                LaneRatio{.Lane = lane, .Ratio = Distribute(std::move(values))});
        }
        std::ranges::stable_sort(
            comparison.Scopes, [](const ScopeComparison& l, const ScopeComparison& r)
            { return std::max(l.MedianMsA, l.MedianMsB) > std::max(r.MedianMsA, r.MedianMsB); });
        return comparison;
    }
}
