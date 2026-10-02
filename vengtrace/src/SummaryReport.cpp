#include "SummaryReport.h"

#include <algorithm>
#include <iterator>

#include <fmt/format.h>
#include <nlohmann/json.hpp>

namespace Veng::VengTrace
{
    namespace
    {
        using Json = nlohmann::json;

        // Scope names run long; a column this wide holds nearly all of them, and a longer one
        // pushes its row's figures right rather than being cut.
        constexpr usize NameWidth = 36;

        string Histogram(const vector<HistogramBin>& bins)
        {
            string text;
            for (const HistogramBin& bin : bins)
            {
                fmt::format_to(std::back_inserter(text), "{}{}x{}", text.empty() ? "" : "  ",
                               bin.Value, bin.Count);
            }
            return text;
        }

        void DistributionRow(string& out, string_view label, const Distribution& distribution)
        {
            fmt::format_to(std::back_inserter(out),
                           "  {:<24}{:>8.2f}{:>8.2f}{:>8.2f}{:>8.2f}{:>9.2f}\n", label,
                           distribution.Median, distribution.Mean, distribution.P90,
                           distribution.P99, distribution.Max);
        }

        Json DistributionJson(const Distribution& distribution)
        {
            return Json{{"count", distribution.Count}, {"min", distribution.Min},
                        {"p10", distribution.P10},     {"median", distribution.Median},
                        {"mean", distribution.Mean},   {"p90", distribution.P90},
                        {"p99", distribution.P99},     {"max", distribution.Max}};
        }

        Json HistogramJson(const vector<HistogramBin>& bins)
        {
            Json histogram = Json::array();
            for (const HistogramBin& bin : bins)
            {
                histogram.push_back(Json{{"value", bin.Value}, {"count", bin.Count}});
            }
            return histogram;
        }
    }

    string FormatSummary(const CaptureSummary& summary, string_view capture)
    {
        string out;
        const auto line = [&out](string_view text) { out += text; };
        fmt::format_to(std::back_inserter(out), "capture  {}{}{}, format {}{}\n", capture,
                       summary.Executable.empty() ? "" : " (",
                       summary.Executable.empty() ? string() : summary.Executable + ")",
                       summary.FormatVersion, summary.Truncated ? ", truncated" : "");
        if (summary.DroppedEvents != 0)
        {
            fmt::format_to(std::back_inserter(out), "         {} events dropped by the producer\n",
                           summary.DroppedEvents);
        }
        if (summary.Frames.Period.Count == 0)
        {
            line("frames   none whole: the main thread recorded fewer than three frames\n");
            return out;
        }
        fmt::format_to(std::back_inserter(out),
                       "frames   {} whole ({}-{}) on thread \"{}\"; the first and last are partial "
                       "and left out\n",
                       summary.Frames.Period.Count, summary.FirstFrame, summary.LastFrame,
                       summary.MainThread);
        if (!summary.ExtraIdleScopes.empty())
        {
            string names;
            for (const string& name : summary.ExtraIdleScopes)
            {
                names += (names.empty() ? "" : ", ") + name;
            }
            fmt::format_to(std::back_inserter(out), "         idle as given: {}\n", names);
        }
        else if (!summary.HasIdleMarks)
        {
            line("         no scope is marked idle, so work is the whole of the scoped time\n");
        }

        line("\nFrames (ms)               median    mean     p90     p99      max\n");
        DistributionRow(out, "period", summary.Frames.Period);
        DistributionRow(out, "work (idle excluded)", summary.Frames.Work);
        DistributionRow(out, "idle", summary.Frames.Idle);

        fmt::format_to(std::back_inserter(out),
                       "\nSteady state: main thread, exclusive ms per frame, top {} by median\n",
                       summary.SteadyState.size());
        fmt::format_to(std::back_inserter(out), "  {:<{}}{:>8}{:>8}{:>9}{:>8}{:>8}\n", "scope",
                       NameWidth, "median", "mean", "worst", "frame", "frames");
        for (const ScopeCost& cost : summary.SteadyState)
        {
            fmt::format_to(std::back_inserter(out), "  {:<{}}{:>8.2f}{:>8.2f}{:>9.2f}{:>8}{:>8}\n",
                           cost.Name, NameWidth, cost.MedianMs, cost.MeanMs, cost.WorstMs,
                           cost.WorstFrame, cost.Frames);
        }

        fmt::format_to(std::back_inserter(out), "\nHitches: the {} longest frames\n",
                       summary.Hitches.size());
        for (const Hitch& hitch : summary.Hitches)
        {
            fmt::format_to(
                std::back_inserter(out),
                "  frame {}: period {:.2f} ms, work {:.2f}, idle {:.2f}, unscoped {:.2f}\n",
                hitch.Frame, hitch.PeriodMs, hitch.WorkMs, hitch.IdleMs, hitch.UnscopedMs);
            line("    top exclusive:\n");
            for (const NamedMs& scope : hitch.TopExclusive)
            {
                fmt::format_to(std::back_inserter(out), "      {:<{}}{:>9.2f}\n", scope.Name,
                               NameWidth, scope.Ms);
            }
            fmt::format_to(std::back_inserter(out),
                           "    tree, scopes of {} ms and over (inclusive / exclusive):\n",
                           summary.TreeMinMs);
            for (const HitchNode& node : hitch.Tree)
            {
                const string indented = string(2 * node.Depth, ' ') + node.Name;
                fmt::format_to(std::back_inserter(out), "      {:<{}}{:>9.2f}{:>9.2f}{}\n",
                               indented, NameWidth, node.InclusiveMs, node.ExclusiveMs,
                               node.Idle ? "  idle" : "");
            }
        }

        fmt::format_to(std::back_inserter(out),
                       "\nCounters\n  {:<{}}{:>8}{:>11}{:>11}{:>11}{:>13}{:>11}\n", "counter",
                       NameWidth, "samples", "min", "median", "max", "frame median", "frame mean");
        for (const CounterSummary& counter : summary.Counters)
        {
            fmt::format_to(std::back_inserter(out),
                           "  {:<{}}{:>8}{:>11.5g}{:>11.5g}{:>11.5g}{:>13.5g}{:>11.5g}\n",
                           counter.Name, NameWidth, counter.Samples.Count, counter.Samples.Min,
                           counter.Samples.Median, counter.Samples.Max, counter.PerFrameSum.Median,
                           counter.PerFrameSum.Mean);
            if (!counter.Histogram.empty())
            {
                fmt::format_to(std::back_inserter(out), "    value x samples: {}\n",
                               Histogram(counter.Histogram));
            }
        }

        for (const GpuSummary& gpu : summary.Gpu)
        {
            fmt::format_to(
                std::back_inserter(out),
                "\nGPU track \"{}\": {} frames (ms)  median    mean     p90     p99      "
                "max\n",
                gpu.Track, gpu.Frame.Count);
            DistributionRow(out, "frame", gpu.Frame);
            DistributionRow(out, "passes' union", gpu.PassUnion);
            DistributionRow(out, "untimed", gpu.Untimed);
            fmt::format_to(std::back_inserter(out), "  passes, top {} by median:\n",
                           gpu.Passes.size());
            for (const GpuPassCost& pass : gpu.Passes)
            {
                fmt::format_to(std::back_inserter(out), "    {:<{}}{:>8.2f}{:>8.2f}{:>8} frames\n",
                               pass.Name, NameWidth, pass.MedianMs, pass.MeanMs, pass.Frames);
            }
            line("  where each frame's largest untimed gap falls:\n");
            for (const GpuGapTally& gap : gpu.LargestGaps)
            {
                fmt::format_to(std::back_inserter(out), "    {:<{}}{:>8} frames\n",
                               gap.Before + " -> " + gap.After, NameWidth + 16, gap.Frames);
            }
        }

        if (!summary.Sim.empty())
        {
            line("\nSimulation per world\n");
        }
        for (const SimWorldSummary& sim : summary.Sim)
        {
            fmt::format_to(std::back_inserter(out),
                           "  {}: {} steps; ms per step median {:.3f}, mean {:.3f}\n"
                           "    steps x frames: {}\n",
                           sim.Scope, sim.Steps, sim.MedianMsPerStep, sim.MeanMsPerStep,
                           Histogram(sim.StepsPerFrame));
        }
        return out;
    }

    string SummaryToJson(const CaptureSummary& summary, string_view capture)
    {
        Json document;
        document["capture"] = string(capture);
        document["executable"] = summary.Executable;
        document["formatVersion"] = summary.FormatVersion;
        document["truncated"] = summary.Truncated;
        document["droppedEvents"] = summary.DroppedEvents;
        document["mainThread"] = summary.MainThread;
        document["hasIdleMarks"] = summary.HasIdleMarks;
        document["extraIdleScopes"] = summary.ExtraIdleScopes;
        document["frames"] = Json{{"first", summary.FirstFrame},
                                  {"last", summary.LastFrame},
                                  {"period", DistributionJson(summary.Frames.Period)},
                                  {"work", DistributionJson(summary.Frames.Work)},
                                  {"idle", DistributionJson(summary.Frames.Idle)}};

        Json steady = Json::array();
        for (const ScopeCost& cost : summary.SteadyState)
        {
            steady.push_back(Json{{"name", cost.Name},
                                  {"median", cost.MedianMs},
                                  {"mean", cost.MeanMs},
                                  {"worst", cost.WorstMs},
                                  {"worstFrame", cost.WorstFrame},
                                  {"frames", cost.Frames}});
        }
        document["steadyState"] = std::move(steady);

        Json hitches = Json::array();
        for (const Hitch& hitch : summary.Hitches)
        {
            Json top = Json::array();
            for (const NamedMs& scope : hitch.TopExclusive)
            {
                top.push_back(Json{{"name", scope.Name}, {"exclusive", scope.Ms}});
            }
            Json tree = Json::array();
            for (const HitchNode& node : hitch.Tree)
            {
                tree.push_back(Json{{"name", node.Name},
                                    {"depth", node.Depth},
                                    {"inclusive", node.InclusiveMs},
                                    {"exclusive", node.ExclusiveMs},
                                    {"idle", node.Idle}});
            }
            hitches.push_back(Json{{"frame", hitch.Frame},
                                   {"period", hitch.PeriodMs},
                                   {"work", hitch.WorkMs},
                                   {"idle", hitch.IdleMs},
                                   {"unscoped", hitch.UnscopedMs},
                                   {"treeMinMs", summary.TreeMinMs},
                                   {"topExclusive", std::move(top)},
                                   {"tree", std::move(tree)}});
        }
        document["hitches"] = std::move(hitches);

        Json counters = Json::array();
        for (const CounterSummary& counter : summary.Counters)
        {
            Json entry{{"name", counter.Name},
                       {"samples", DistributionJson(counter.Samples)},
                       {"perFrameSum", DistributionJson(counter.PerFrameSum)}};
            if (!counter.Histogram.empty())
            {
                entry["histogram"] = HistogramJson(counter.Histogram);
            }
            counters.push_back(std::move(entry));
        }
        document["counters"] = std::move(counters);

        Json gpus = Json::array();
        for (const GpuSummary& gpu : summary.Gpu)
        {
            Json passes = Json::array();
            for (const GpuPassCost& pass : gpu.Passes)
            {
                passes.push_back(Json{{"name", pass.Name},
                                      {"frames", pass.Frames},
                                      {"median", pass.MedianMs},
                                      {"mean", pass.MeanMs}});
            }
            Json gaps = Json::array();
            for (const GpuGapTally& gap : gpu.LargestGaps)
            {
                gaps.push_back(
                    Json{{"before", gap.Before}, {"after", gap.After}, {"frames", gap.Frames}});
            }
            gpus.push_back(Json{{"track", gpu.Track},
                                {"frame", DistributionJson(gpu.Frame)},
                                {"passUnion", DistributionJson(gpu.PassUnion)},
                                {"untimed", DistributionJson(gpu.Untimed)},
                                {"passes", std::move(passes)},
                                {"largestGaps", std::move(gaps)}});
        }
        document["gpu"] = std::move(gpus);

        Json sims = Json::array();
        for (const SimWorldSummary& sim : summary.Sim)
        {
            sims.push_back(Json{{"scope", sim.Scope},
                                {"steps", sim.Steps},
                                {"stepsPerFrame", HistogramJson(sim.StepsPerFrame)},
                                {"medianMsPerStep", sim.MedianMsPerStep},
                                {"meanMsPerStep", sim.MeanMsPerStep}});
        }
        document["sim"] = std::move(sims);
        return document.dump(2) + "\n";
    }

    string FormatComparison(const Comparison& comparison, string_view a, string_view b, u32 top)
    {
        string out;
        fmt::format_to(std::back_inserter(out),
                       "a  {}\nb  {}\n{} scopes in both ({} only in a, {} only in b); per-call "
                       "median ms over each capture's whole frames\n",
                       a, b, comparison.Scopes.size(), comparison.OnlyInA, comparison.OnlyInB);
        for (const LaneRatio& lane : comparison.Ratios)
        {
            const Distribution& ratio = lane.Ratio;
            fmt::format_to(
                std::back_inserter(out),
                "a/b, {} scopes of {} ms and over in both ({}): median {:.2f}, p10 {:.2f}, "
                "p90 {:.2f}, min {:.2f}, max {:.2f}\n",
                lane.Lane.empty() ? "CPU" : lane.Lane, comparison.RatioFloorMs, ratio.Count,
                ratio.Median, ratio.P10, ratio.P90, ratio.Min, ratio.Max);
        }
        fmt::format_to(std::back_inserter(out), "\n  {:<{}}{:>8}{:>10}{:>8}{:>10}{:>8}\n", "scope",
                       NameWidth, "calls a", "median a", "calls b", "median b", "a/b");
        const usize rows =
            top == 0 ? comparison.Scopes.size() : std::min<usize>(top, comparison.Scopes.size());
        for (usize row = 0; row < rows; ++row)
        {
            const ScopeComparison& scope = comparison.Scopes[row];
            const string ratioText =
                scope.Ratio ? fmt::format("{:.2f}", *scope.Ratio) : string("-");
            const string name =
                scope.Lane.empty() ? scope.Name : fmt::format("{} [{}]", scope.Name, scope.Lane);
            fmt::format_to(std::back_inserter(out), "  {:<{}}{:>8}{:>10.3f}{:>8}{:>10.3f}{:>8}\n",
                           name, NameWidth, scope.CallsA, scope.MedianMsA, scope.CallsB,
                           scope.MedianMsB, ratioText);
        }
        return out;
    }

    string ComparisonToJson(const Comparison& comparison, string_view a, string_view b)
    {
        Json scopes = Json::array();
        for (const ScopeComparison& scope : comparison.Scopes)
        {
            Json row{{"name", scope.Name},     {"lane", scope.Lane},
                     {"callsA", scope.CallsA}, {"medianA", scope.MedianMsA},
                     {"callsB", scope.CallsB}, {"medianB", scope.MedianMsB}};
            row["ratio"] = scope.Ratio ? Json(*scope.Ratio) : Json(nullptr);
            scopes.push_back(std::move(row));
        }
        Json ratios = Json::array();
        for (const LaneRatio& lane : comparison.Ratios)
        {
            ratios.push_back(Json{{"lane", lane.Lane}, {"ratio", DistributionJson(lane.Ratio)}});
        }
        const Json document{{"a", string(a)},
                            {"b", string(b)},
                            {"onlyInA", comparison.OnlyInA},
                            {"onlyInB", comparison.OnlyInB},
                            {"ratioFloorMs", comparison.RatioFloorMs},
                            {"ratios", std::move(ratios)},
                            {"scopes", std::move(scopes)}};
        return document.dump(2) + "\n";
    }
}
