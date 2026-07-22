// Device-free performance-panel logic. The panel itself draws through ImGui and a device, but its
// data-shaping is pure: the heaviest-N phase selection (with hysteresis), the GPU band fold into
// "Other" (total-preserving), the scope-table sort comparators (total, tie-broken by name), the
// rolling history ring wrap, and the profiler-availability notice that keeps the panel from ever
// degrading to a silent empty table. None of these touch a viewport or a GPU.

#include <doctest/doctest.h>

#include <Veng/Diagnostics/Profiler.h>
#include <Veng/UI/DebugPanels.h>

#include <algorithm>
#include <numeric>

using namespace Veng;
using namespace Veng::UI;

namespace
{
    f32 SumMs(const vector<GpuBand>& bands)
    {
        return std::accumulate(bands.begin(), bands.end(), 0.0f, [](f32 acc, const GpuBand& band)
                               { return acc + band.Milliseconds; });
    }

    bool Contains(const vector<string>& names, string_view name)
    {
        return std::ranges::find(names, name) != names.end();
    }
}

TEST_CASE("PerformanceHistory ring wraps without losing its sample count")
{
    PerformanceHistory ring;
    const usize pushed = PerformanceHistory::Capacity + 60;
    for (usize i = 0; i < pushed; i++)
    {
        ring.Push(static_cast<f32>(i));
    }
    CHECK(ring.Count == PerformanceHistory::Capacity);
    CHECK(ring.Last() == doctest::Approx(static_cast<f32>(pushed - 1)));
    CHECK(ring.PlotOffset() == static_cast<i32>(ring.Head));

    // Before it wraps, samples plot in array order (offset 0).
    PerformanceHistory partial;
    partial.Push(1.0f);
    partial.Push(2.0f);
    CHECK(partial.Count == 2);
    CHECK(partial.PlotOffset() == 0);
    CHECK(partial.Last() == doctest::Approx(2.0f));
}

TEST_CASE("SelectHeaviestPhases picks the N heaviest, deterministically")
{
    const vector<PhaseSample> candidates = {{.Name = "Frame/A", .InclusiveMs = 5.0},
                                            {.Name = "Frame/B", .InclusiveMs = 3.0},
                                            {.Name = "Frame/C", .InclusiveMs = 9.0},
                                            {.Name = "Frame/D", .InclusiveMs = 1.0},
                                            {.Name = "Frame/E", .InclusiveMs = 7.0}};
    vector<string> selection;
    SelectHeaviestPhases(candidates, selection, 3, 0.25f);
    REQUIRE(selection.size() == 3);

    vector<string> sorted = selection;
    std::ranges::sort(sorted);
    CHECK(sorted == vector<string>{"Frame/A", "Frame/C", "Frame/E"});

    // Fewer candidates than slots selects them all.
    const vector<PhaseSample> few = {{.Name = "Frame/A", .InclusiveMs = 1.0}};
    vector<string> small;
    SelectHeaviestPhases(few, small, 3, 0.25f);
    CHECK(small.size() == 1);
}

TEST_CASE("SelectHeaviestPhases holds an incumbent through a near-tie")
{
    vector<string> selection = {"Frame/A", "Frame/B"};

    // C nudges marginally above the incumbent B but within the hysteresis margin: B keeps its slot.
    const vector<PhaseSample> nearTie = {{.Name = "Frame/A", .InclusiveMs = 10.0},
                                         {.Name = "Frame/B", .InclusiveMs = 9.0},
                                         {.Name = "Frame/C", .InclusiveMs = 9.1}};
    SelectHeaviestPhases(nearTie, selection, 2, 0.25f);
    REQUIRE(selection.size() == 2);
    CHECK(Contains(selection, "Frame/B"));
    CHECK_FALSE(Contains(selection, "Frame/C"));

    // B collapses well below the cutoff: the challenger now displaces it.
    const vector<PhaseSample> collapse = {{.Name = "Frame/A", .InclusiveMs = 10.0},
                                          {.Name = "Frame/B", .InclusiveMs = 1.0},
                                          {.Name = "Frame/C", .InclusiveMs = 9.0}};
    SelectHeaviestPhases(collapse, selection, 2, 0.25f);
    REQUIRE(selection.size() == 2);
    CHECK(Contains(selection, "Frame/C"));
    CHECK_FALSE(Contains(selection, "Frame/B"));
}

TEST_CASE("FoldGpuBands folds the surplus into Other, preserving the total")
{
    const vector<GpuBand> passes = {
        {.Name = "p0", .Milliseconds = 4.0f},  {.Name = "p1", .Milliseconds = 3.0f},
        {.Name = "p2", .Milliseconds = 2.0f},  {.Name = "p3", .Milliseconds = 1.5f},
        {.Name = "p4", .Milliseconds = 1.0f},  {.Name = "p5", .Milliseconds = 0.5f},
        {.Name = "p6", .Milliseconds = 0.25f}, {.Name = "p7", .Milliseconds = 0.1f}};
    const f32 total = SumMs(passes);

    const vector<GpuBand> folded = FoldGpuBands(passes, 6);
    REQUIRE(folded.size() == 6);
    CHECK(folded.back().Name == "Other");
    CHECK(folded.front().Name == "p0");
    CHECK(folded[4].Name == "p4");
    // The band cap never loses time: the folded total equals the un-capped total.
    CHECK(SumMs(folded) == doctest::Approx(total));

    // Within the cap, the passes pass through unchanged (sorted heaviest-first), no Other band.
    const vector<GpuBand> fit = {{.Name = "a", .Milliseconds = 2.0f},
                                 {.Name = "b", .Milliseconds = 1.0f}};
    const vector<GpuBand> kept = FoldGpuBands(fit, 6);
    REQUIRE(kept.size() == 2);
    CHECK(kept[0].Name == "a");
    CHECK(kept[1].Name == "b");
    CHECK(SumMs(kept) == doctest::Approx(SumMs(fit)));
}

TEST_CASE("SortScopeRows orders every column and breaks ties by name")
{
    const vector<ScopeRow> base = {
        {.Name = "beta", .InclusiveMs = 2.0, .SelfMs = 1.0, .PercentOfFrame = 20.0, .CallCount = 5},
        {.Name = "alpha",
         .InclusiveMs = 2.0,
         .SelfMs = 3.0,
         .PercentOfFrame = 20.0,
         .CallCount = 5},
        {.Name = "gamma",
         .InclusiveMs = 1.0,
         .SelfMs = 0.5,
         .PercentOfFrame = 10.0,
         .CallCount = 9}};

    // Inclusive descending: alpha/beta tie at 2.0 → name ascending, then gamma.
    vector<ScopeRow> rows = base;
    SortScopeRows(rows, ScopeSortColumn::Inclusive, false);
    CHECK(rows[0].Name == "alpha");
    CHECK(rows[1].Name == "beta");
    CHECK(rows[2].Name == "gamma");

    // Calls ascending: 5,5,9 → the tie holds the name order, gamma last.
    rows = base;
    SortScopeRows(rows, ScopeSortColumn::Calls, true);
    CHECK(rows[0].Name == "alpha");
    CHECK(rows[1].Name == "beta");
    CHECK(rows[2].Name == "gamma");

    // Self descending: alpha(3) > beta(1) > gamma(0.5), no ties.
    rows = base;
    SortScopeRows(rows, ScopeSortColumn::Self, false);
    CHECK(rows[0].Name == "alpha");
    CHECK(rows[2].Name == "gamma");

    // Percent ascending: gamma(10) then the alpha/beta(20) tie by name.
    rows = base;
    SortScopeRows(rows, ScopeSortColumn::Percent, true);
    CHECK(rows[0].Name == "gamma");
    CHECK(rows[1].Name == "alpha");
    CHECK(rows[2].Name == "beta");

    // Name column both directions.
    rows = base;
    SortScopeRows(rows, ScopeSortColumn::Name, true);
    CHECK(rows[0].Name == "alpha");
    CHECK(rows[2].Name == "gamma");
    SortScopeRows(rows, ScopeSortColumn::Name, false);
    CHECK(rows[0].Name == "gamma");
    CHECK(rows[2].Name == "alpha");
}

TEST_CASE("PerformanceProfilerNotice never yields a silent empty table")
{
#if defined(VE_PROFILE) && VE_PROFILE
    // With the profiler compiled in, the notice tracks whether an instance is installed. Only touch
    // the installed branch when the process has no active profiler, since a second is unsupported.
    if (Diagnostics::GetActiveProfiler() == nullptr)
    {
        CHECK_FALSE(PerformanceProfilerNotice().empty());
        {
            const Diagnostics::Profiler profiler;
            CHECK(PerformanceProfilerNotice().empty());
        }
        CHECK_FALSE(PerformanceProfilerNotice().empty());
    }
#else
    // Compiled out: always the disabled notice, never empty — the degraded branch states the reason.
    CHECK_FALSE(PerformanceProfilerNotice().empty());
#endif
}
