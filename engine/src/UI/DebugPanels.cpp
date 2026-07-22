#include <Veng/UI/DebugPanels.h>

#include <Veng/Renderer/Context.h>
#include <Veng/Renderer/Image.h>
#include <Veng/Renderer/ImageView.h>
#include <Veng/Renderer/SceneRenderer.h>
#include <Veng/Renderer/DofTile.h>
#include <Veng/Renderer/Viewport.h>
#include <Veng/Scene/SceneViewport.h>
#include <Veng/UI/Layout.h>
#include <Veng/UI/Query.h>
#include <Veng/UI/Scopes.h>
#include <Veng/UI/Widgets.h>

#include <Veng/Diagnostics/Profiler.h>
#include <Veng/Time.h>

#include <algorithm>
#include <cstddef>
#include <ranges>
#include <span>

namespace Veng::UI
{
    using Renderer::SceneRendererSettings;
    using Renderer::Viewport;

    void RendererStatsPanel(const Viewport& viewport)
    {
        const Renderer::SceneRenderer& renderer = viewport.GetRenderer();
        const Renderer::Context& context = renderer.GetOutput()->GetImage()->GetContext();

        UI::Text(fmt::format("{:.1f} fps ({:.2f} ms)", UI::FrameRate(), 1000.0f / UI::FrameRate()));

        if (context.IsGpuTimingSupported())
        {
            UI::Text(fmt::format("GPU frame: {:.2f} ms", context.GetLastGpuFrameTimeMs()));
        }

        // The live sub-rect scale (the inner loop writes it while dynamic resolution is on) and
        // the rendered extent, which shrinks with the scale while the window stays full size.
        UI::Text(fmt::format("Render scale: {:.2f}{}", viewport.GetRenderScale(),
                             viewport.IsDynamicResolutionEnabled() ? " (auto)" : ""));
        const Ref<Renderer::Image> target = renderer.GetOutput()->GetImage();
        UI::Text(fmt::format("Render target: {}x{}", target->GetWidth(), target->GetHeight()));

        // The fixed allocation the targets are sized at: its scale and extent. The per-frame
        // sub-rect renders inside this and the tonemap upscales it to the full output, so the
        // allocation extent holds steady while the rendered sub-rect shrinks with the scale.
        const uvec2 allocExtent = viewport.GetAllocationExtent();
        UI::Text(fmt::format("Allocation: {:.2f}", viewport.GetAllocationScale()));
        UI::Text(fmt::format("Allocation extent: {}x{}", allocExtent.x, allocExtent.y));

        // The cull funnel: gathered submesh candidates → frustum survivors → draws issued.
        UI::Text(fmt::format("Gathered: {}", renderer.GetLastVisibleCount()));
        UI::Text(fmt::format("Frustum survived: {}", renderer.GetFrustumSurvivedCount()));
        UI::Text(fmt::format("Drawn: {}", renderer.GetLastDrawnCount()));

        // Under the GPU path the occlusion test zeroes occluded commands' instanceCount; the
        // survivor count is read back one frame late. The active line shows the real mode (GPU
        // degrades to CPU on a device without multiDrawIndirect).
        const bool gpuActive = renderer.GetActiveCullMode() == SceneRendererSettings::CullMode::GPU;
        UI::Text(fmt::format("Cull mode: {}", gpuActive ? "GPU" : "CPU"));
        if (gpuActive)
        {
            UI::Text(fmt::format("Occlusion survived: {}", renderer.GetLastGpuSurvivorCount()));
        }

        const bool rebuilt = renderer.DidBroadphaseRebuildLastFrame();
        UI::Text(fmt::format("Broadphase: {} ({} nodes)", rebuilt ? "rebuilt" : "static",
                             renderer.GetBroadphaseNodeCount()));
    }

    namespace
    {
        // One pass's GPU time after grouping: the bloom and hi-Z mip sweeps each collapse to a
        // single named entry, every other pass stays itself.
        struct PassCost
        {
            string Name;
            f32 Milliseconds = 0.0f;
        };

        // Folds a per-scope pass name onto its display group: the per-mip bloom and hi-Z passes
        // ("Bloom Down Mip 3", "HiZ Reduce Mip 2", …) collapse to "Bloom" / "Hi-Z" so the sweep
        // reads as one pass; any other name passes through unchanged.
        string PassGroup(string_view name)
        {
            if (name.starts_with("Bloom"))
            {
                return "Bloom";
            }
            if (name.starts_with("HiZ") || name.starts_with("Hi-Z"))
            {
                return "Hi-Z";
            }
            return string(name);
        }

        // Aggregates the frame's per-scope timings into grouped pass costs, summing each group's
        // contiguous mip passes and preserving first-seen (execution) order.
        vector<PassCost> AggregatePasses(std::span<const Renderer::Context::GpuPassTiming> passes)
        {
            vector<PassCost> costs;
            for (const Renderer::Context::GpuPassTiming& pass : passes)
            {
                string group = PassGroup(pass.Name);
                const auto existing = std::ranges::find(costs, group, &PassCost::Name);
                if (existing == costs.end())
                {
                    costs.push_back({.Name = std::move(group), .Milliseconds = pass.Milliseconds});
                }
                else
                {
                    existing->Milliseconds += pass.Milliseconds;
                }
            }
            return costs;
        }

        // A categorical palette indexed by chart slot, so slot 0 keeps its color across frames even
        // as the phase or band occupying it changes: widely separated hues with alternating
        // brightness so neighbors in the legend stay distinguishable.
        constexpr std::array<vec4, 8> SlotPalette{
            vec4{0.90f, 0.10f, 0.10f, 1.0f}, // red
            vec4{1.00f, 0.85f, 0.00f, 1.0f}, // yellow
            vec4{0.10f, 0.80f, 0.30f, 1.0f}, // green
            vec4{1.00f, 0.45f, 0.00f, 1.0f}, // orange
            vec4{0.55f, 0.20f, 0.90f, 1.0f}, // violet
            vec4{0.10f, 0.85f, 0.90f, 1.0f}, // cyan
            vec4{0.95f, 0.35f, 0.70f, 1.0f}, // pink
            vec4{0.60f, 0.80f, 0.10f, 1.0f}, // lime
        };

        // The color for chart slot i, wrapping the palette.
        vec4 SlotColor(usize i)
        {
            return SlotPalette[i % SlotPalette.size()];
        }

        // The whole-frame GPU total line is white; the CPU total a light blue; slots take the palette.
        constexpr vec4 GpuLineColor{1.0f, 1.0f, 1.0f, 1.0f};
        constexpr vec4 CpuLineColor{0.65f, 0.80f, 1.0f, 1.0f};
    }

    void PerformanceHistory::Push(const f32 milliseconds)
    {
        Samples[Head] = milliseconds;
        Head = (Head + 1) % Capacity;
        Count = std::min(Count + 1, Capacity);
    }

    f32 PerformanceHistory::Last() const
    {
        return Count == 0 ? 0.0f : Samples[(Head + Capacity - 1) % Capacity];
    }

    i32 PerformanceHistory::PlotOffset() const
    {
        // A full buffer wraps, so the oldest sample sits at the write head; until then it is
        // filled from index 0 and plots in array order.
        return Count == Capacity ? static_cast<i32>(Head) : 0;
    }

    void SelectHeaviestPhases(std::span<const PhaseSample> candidates, vector<string>& selection,
                              const usize count, const f32 hysteresis)
    {
        // Rank candidates heaviest-first, ties by name for a total order.
        vector<PhaseSample> ranked(candidates.begin(), candidates.end());
        std::ranges::sort(ranked,
                          [](const PhaseSample& a, const PhaseSample& b)
                          {
                              return a.InclusiveMs != b.InclusiveMs ? a.InclusiveMs > b.InclusiveMs
                                                                    : a.Name < b.Name;
                          });

        // The cost of the count-th heaviest is the bar an incumbent must stay near to keep its slot;
        // only a challenger clearing it by the hysteresis margin displaces the incumbent.
        const f64 cutoff = ranked.size() >= count ? ranked[count - 1].InclusiveMs : 0.0;
        const f64 threshold = cutoff * (1.0 - static_cast<f64>(hysteresis));

        const auto costOf = [&](string_view name) -> optional<f64>
        {
            for (const PhaseSample& sample : ranked)
            {
                if (sample.Name == name)
                {
                    return sample.InclusiveMs;
                }
            }
            return std::nullopt;
        };

        vector<string> next;
        next.reserve(count);
        // Surviving incumbents keep their slots, in their existing display order.
        for (const string& name : selection)
        {
            if (next.size() >= count)
            {
                break;
            }
            const optional<f64> cost = costOf(name);
            if (cost.has_value() && *cost >= threshold)
            {
                next.push_back(name);
            }
        }
        // Fill the remaining slots heaviest-first, skipping those already kept.
        for (const PhaseSample& sample : ranked)
        {
            if (next.size() >= count)
            {
                break;
            }
            if (std::ranges::find(next, sample.Name) == next.end())
            {
                next.push_back(sample.Name);
            }
        }
        selection = std::move(next);
    }

    vector<GpuBand> FoldGpuBands(std::span<const GpuBand> passes, const usize maxBands)
    {
        vector<GpuBand> sorted(passes.begin(), passes.end());
        std::ranges::sort(sorted,
                          [](const GpuBand& a, const GpuBand& b)
                          {
                              return a.Milliseconds != b.Milliseconds
                                         ? a.Milliseconds > b.Milliseconds
                                         : a.Name < b.Name;
                          });
        if (maxBands == 0 || sorted.size() <= maxBands)
        {
            return sorted;
        }

        // Keep the heaviest maxBands-1 and fold the rest into one "Other" band, so the returned
        // total equals the input total exactly — the band cap never loses time.
        vector<GpuBand> folded(sorted.begin(),
                               sorted.begin() + static_cast<std::ptrdiff_t>(maxBands - 1));
        f32 other = 0.0f;
        for (usize i = maxBands - 1; i < sorted.size(); i++)
        {
            other += sorted[i].Milliseconds;
        }
        folded.push_back({.Name = "Other", .Milliseconds = other});
        return folded;
    }

    void SortScopeRows(vector<ScopeRow>& rows, const ScopeSortColumn column, const bool ascending)
    {
        const auto compare = [column, ascending](const ScopeRow& a, const ScopeRow& b)
        {
            const auto ordered = [ascending](auto lhs, auto rhs)
            { return ascending ? lhs < rhs : lhs > rhs; };
            switch (column)
            {
            case ScopeSortColumn::Name:
                return ascending ? a.Name < b.Name : a.Name > b.Name;
            case ScopeSortColumn::Inclusive:
                if (a.InclusiveMs != b.InclusiveMs)
                {
                    return ordered(a.InclusiveMs, b.InclusiveMs);
                }
                break;
            case ScopeSortColumn::Self:
                if (a.SelfMs != b.SelfMs)
                {
                    return ordered(a.SelfMs, b.SelfMs);
                }
                break;
            case ScopeSortColumn::Percent:
                if (a.PercentOfFrame != b.PercentOfFrame)
                {
                    return ordered(a.PercentOfFrame, b.PercentOfFrame);
                }
                break;
            case ScopeSortColumn::Calls:
                if (a.CallCount != b.CallCount)
                {
                    return ordered(a.CallCount, b.CallCount);
                }
                break;
            }
            // Every non-name column breaks ties by name, so the order is total and stable.
            return a.Name < b.Name;
        };
        std::ranges::sort(rows, compare);
    }

    string_view PerformanceProfilerNotice()
    {
#if defined(VE_PROFILE) && VE_PROFILE
        if (Diagnostics::GetActiveProfiler() != nullptr)
        {
            return {};
        }
        return "No active profiler — per-scope timings unavailable.";
#else
        return "Profiling is disabled in this build (VE_PROFILE=OFF); rebuild with "
               "-DVE_PROFILE=ON for per-scope timings.";
#endif
    }

    namespace
    {
        // The frame budget the budget bar and charts measure against: one 60 Hz frame.
        constexpr f32 FrameBudgetMs = 1000.0f / 60.0f;

        // Draws a color swatch followed by "label: value ms" on the current line — one legend entry.
        void LegendEntry(vec4 color, string_view label, f32 milliseconds)
        {
            const f32 swatch = UI::GetTextLineHeight();
            UI::Badge("", color, {swatch, swatch});
            UI::SameLine();
            UI::Text(fmt::format("{}: {:.3f} ms", label, milliseconds));
        }
    }

    void PerformancePanel::Draw(const Viewport& viewport)
    {
        const Renderer::SceneRenderer& renderer = viewport.GetRenderer();
        const Renderer::Context& context = renderer.GetOutput()->GetImage()->GetContext();
        const bool gpuTiming = context.IsGpuTimingSupported();

        // The always-available totals — CPU frame delta from Time, GPU whole-frame from Context —
        // neither of which depends on the profiler, so the budget bar and totals draw in any build.
        const f32 cpuMs = Time::GetDeltaTime() * 1000.0f;
        const f32 gpuMs = gpuTiming ? context.GetLastGpuFrameTimeMs() : 0.0f;

        // Profiler-sourced data: the per-frame per-scope aggregates. Null under VE_PROFILE=OFF, or
        // when no profiler is installed — the phase series and scope table degrade to the notice.
        Diagnostics::Profiler* profiler = Diagnostics::GetActiveProfiler();
        const string_view notice = PerformanceProfilerNotice();

        // Roll the frame's aggregates into table rows and the phase-selection candidates. The frame
        // wall time is the % denominator; guard against a zero first frame.
        const f64 frameMs = std::max(static_cast<f64>(cpuMs), 1.0e-4);
        vector<ScopeRow> rows;
        vector<PhaseSample> phaseCandidates;
        if (profiler != nullptr)
        {
            for (const Diagnostics::ScopeAggregate& agg : profiler->GetFrameAggregates())
            {
                if (agg.CallCount == 0)
                {
                    continue; // Ran in an earlier frame, not this one.
                }
                const string_view name = profiler->GetName(agg.Name);
                if (name.empty())
                {
                    continue;
                }
                const f64 inclusiveMs = static_cast<f64>(agg.InclusiveNanos) * 1.0e-6;
                const f64 selfMs = static_cast<f64>(agg.SelfNanos) * 1.0e-6;
                rows.push_back({.Name = string(name),
                                .InclusiveMs = inclusiveMs,
                                .SelfMs = selfMs,
                                .PercentOfFrame = inclusiveMs / frameMs * 100.0,
                                .CallCount = agg.CallCount});
                if (name.starts_with("Frame/"))
                {
                    phaseCandidates.push_back({.Name = string(name), .InclusiveMs = inclusiveMs});
                }
            }
        }

        // Sample the rolling histories. The phase and GPU-band slots each follow whichever
        // phase/band currently occupies them; the identity is settled with hysteresis so a near-tie
        // does not flip a slot frame to frame.
        m_Cpu.Push(cpuMs);
        if (gpuTiming)
        {
            m_Gpu.Push(gpuMs);
        }

        SelectHeaviestPhases(phaseCandidates, m_PhaseNames, PhaseSlots, PhaseHysteresis);
        const auto phaseCost = [&](string_view name)
        {
            for (const PhaseSample& sample : phaseCandidates)
            {
                if (sample.Name == name)
                {
                    return static_cast<f32>(sample.InclusiveMs);
                }
            }
            return 0.0f;
        };
        for (usize i = 0; i < PhaseSlots; i++)
        {
            m_PhaseSlots[i].Push(i < m_PhaseNames.size() ? phaseCost(m_PhaseNames[i]) : 0.0f);
        }

        vector<GpuBand> folded;
        if (gpuTiming)
        {
            vector<GpuBand> bands;
            for (const PassCost& pass : AggregatePasses(context.GetLastGpuPassTimings()))
            {
                bands.push_back({.Name = pass.Name, .Milliseconds = pass.Milliseconds});
            }
            folded = FoldGpuBands(bands, GpuBandSlots);
        }
        m_GpuBandNames.clear();
        for (const GpuBand& band : folded)
        {
            m_GpuBandNames.push_back(band.Name);
        }
        for (usize i = 0; i < GpuBandSlots; i++)
        {
            m_GpuSlots[i].Push(i < folded.size() ? folded[i].Milliseconds : 0.0f);
        }

        // ---- Band 1: the budget bar ----------------------------------------------------------
        {
            const f32 frameCost = std::max(cpuMs, gpuMs);
            const f32 fraction = FrameBudgetMs > 0.0f ? frameCost / FrameBudgetMs : 0.0f;
            const char* side = cpuMs > FrameBudgetMs   ? " CPU-bound"
                               : gpuMs > FrameBudgetMs ? " GPU-bound"
                                                       : "";
            const string frameLabel = profiler != nullptr
                                          ? fmt::format("frame {}", profiler->GetFrameIndex())
                                          : "frame -";
            UI::Text(fmt::format("{:.1f} FPS  |  {}  |  budget {:.2f} ms", UI::FrameRate(),
                                 frameLabel, FrameBudgetMs));
            UI::ProgressBar(fraction, {-1.0f, 0.0f},
                            fmt::format("CPU {:.2f} / GPU {:.2f} ms{}", cpuMs, gpuMs, side));
        }

        // ---- Band 2: the frame-history chart -------------------------------------------------
        UI::SeparatorText("Frame history");
        {
            // A series count fixed by construction — CPU total, GPU total, and the phase slots — so
            // the legend never grows however many phases the frame gains.
            vector<UI::PlotSeries> series;
            series.push_back({.Color = CpuLineColor,
                              .Values = {m_Cpu.Samples.data(), m_Cpu.Count},
                              .Offset = m_Cpu.PlotOffset()});
            if (gpuTiming)
            {
                series.push_back({.Color = GpuLineColor,
                                  .Values = {m_Gpu.Samples.data(), m_Gpu.Count},
                                  .Offset = m_Gpu.PlotOffset()});
            }
            for (usize i = 0; i < m_PhaseNames.size(); i++)
            {
                series.push_back({.Color = SlotColor(i),
                                  .Values = {m_PhaseSlots[i].Samples.data(), m_PhaseSlots[i].Count},
                                  .Offset = m_PhaseSlots[i].PlotOffset()});
            }
            UI::PlotLinesMulti("##framehistory", series,
                               {.ScaleMin = 0.0f, .Size = {0.0f, 110.0f}});

            LegendEntry(CpuLineColor, "CPU", m_Cpu.Last());
            if (gpuTiming)
            {
                UI::SameLine();
                LegendEntry(GpuLineColor, "GPU", m_Gpu.Last());
            }
            for (usize i = 0; i < m_PhaseNames.size(); i++)
            {
                LegendEntry(SlotColor(i), m_PhaseNames[i], m_PhaseSlots[i].Last());
            }
        }

        // ---- Band 3: the GPU pass breakdown --------------------------------------------------
        UI::SeparatorText("GPU pass breakdown");
        if (gpuTiming && !m_GpuBandNames.empty())
        {
            // A stacked read: each band's plotted line is the running sum of the bands beneath it,
            // so the topmost line is the frame's GPU total and a band's share is the gap below it.
            const usize count = m_GpuSlots[0].Count;
            const i32 offset = m_GpuSlots[0].PlotOffset();
            const usize bandCount = std::min(m_GpuBandNames.size(), GpuBandSlots);
            std::array<vector<f32>, GpuBandSlots> stacked;
            for (usize k = 0; k < bandCount; k++)
            {
                stacked[k].resize(count);
                for (usize s = 0; s < count; s++)
                {
                    f32 sum = 0.0f;
                    for (usize j = 0; j <= k; j++)
                    {
                        sum += m_GpuSlots[j].Samples[s];
                    }
                    stacked[k][s] = sum;
                }
            }
            vector<UI::PlotSeries> series;
            for (usize k = 0; k < bandCount; k++)
            {
                series.push_back({.Color = SlotColor(k), .Values = stacked[k], .Offset = offset});
            }
            UI::PlotLinesMulti("##gpustack", series, {.ScaleMin = 0.0f, .Size = {0.0f, 110.0f}});

            for (usize k = 0; k < bandCount; k++)
            {
                LegendEntry(SlotColor(k), m_GpuBandNames[k], m_GpuSlots[k].Last());
            }
        }
        else
        {
            UI::TextDisabled("GPU timing unsupported on this device");
        }

        // ---- Band 4: the sortable scope table ------------------------------------------------
        UI::SeparatorText("Scopes");
        if (!notice.empty())
        {
            // Never a silent empty table: state that per-scope data is unavailable and why.
            UI::TextDisabled(notice);
        }
        else
        {
            (void)UI::InputTextWithHint("##scopefilter", "filter scopes", m_Filter);
            if (!m_Filter.empty())
            {
                std::erase_if(rows, [this](const ScopeRow& row)
                              { return row.Name.find(m_Filter) == string::npos; });
            }
            SortScopeRows(rows, m_SortColumn, m_SortAscending);

            const auto header = [this](string_view label, ScopeSortColumn column)
            {
                UI::TableNextColumn();
                string caption(label);
                if (m_SortColumn == column)
                {
                    caption += m_SortAscending ? " ^" : " v";
                }
                if (UI::Selectable(caption))
                {
                    if (m_SortColumn == column)
                    {
                        m_SortAscending = !m_SortAscending;
                    }
                    else
                    {
                        m_SortColumn = column;
                        m_SortAscending = false;
                    }
                }
            };

            if (auto table = UI::Table("ScopeTable", 5))
            {
                header("Scope", ScopeSortColumn::Name);
                header("Incl ms", ScopeSortColumn::Inclusive);
                header("Self ms", ScopeSortColumn::Self);
                header("% frame", ScopeSortColumn::Percent);
                header("Calls", ScopeSortColumn::Calls);
                for (const ScopeRow& row : rows)
                {
                    UI::TableNextColumn();
                    UI::Text(row.Name);
                    UI::TableNextColumn();
                    UI::Text(fmt::format("{:.3f}", row.InclusiveMs));
                    UI::TableNextColumn();
                    UI::Text(fmt::format("{:.3f}", row.SelfMs));
                    UI::TableNextColumn();
                    UI::Text(fmt::format("{:.1f}", row.PercentOfFrame));
                    UI::TableNextColumn();
                    UI::Text(fmt::format("{}", row.CallCount));
                }
            }
        }

        // ---- Band 5: the counter strip -------------------------------------------------------
        UI::SeparatorText("Counters");
        {
            const bool gpuCull =
                renderer.GetActiveCullMode() == SceneRendererSettings::CullMode::GPU;
            string line = fmt::format("Draws {}  |  Visible {}  |  Frustum survived {}",
                                      renderer.GetLastDrawnCount(), renderer.GetLastVisibleCount(),
                                      renderer.GetFrustumSurvivedCount());
            if (gpuCull)
            {
                line +=
                    fmt::format("  |  Occlusion survived {}", renderer.GetLastGpuSurvivorCount());
            }
            UI::Text(line);
        }
    }

    namespace
    {
        // Draws the viewport's adaptive-resolution and manual render-scale controls. These drive
        // the viewport imperatively (SetDynamicResolution/ClearDynamicResolution/SetRenderScale),
        // recreating or resizing renderer resources directly rather than through the Configure
        // recompile RenderSettingsEditor reports. The dynamic-resolution checkbox greys out without
        // device timestamp support.
        void DrawResolutionControls(Viewport& viewport, const Renderer::Context& context)
        {
            bool dynamic = viewport.IsDynamicResolutionEnabled();
            {
                auto timingDisabled = UI::Disabled(!context.IsGpuTimingSupported());
                if (UI::Checkbox("Dynamic resolution", dynamic))
                {
                    if (dynamic)
                    {
                        viewport.SetDynamicResolution(Renderer::DynamicResolutionSettings{});
                    }
                    else
                    {
                        viewport.ClearDynamicResolution();
                    }
                }
            }

            // The controller's tuning: an edit re-applies through SetDynamicResolution, live on the
            // next frame. Greyed out while dynamic resolution is off.
            {
                auto dynamicOff = UI::Disabled(!viewport.IsDynamicResolutionEnabled());
                Renderer::DynamicResolutionSettings settings =
                    viewport.GetDynamicResolution().value_or(Renderer::DynamicResolutionSettings{});
                bool edited = false;
                edited |= UI::Drag("Target frame time (ms)", settings.TargetFrameTimeMs,
                                   {.Speed = 0.1f, .Min = 1.0f, .Max = 100.0f, .Format = "%.2f"});
                edited |= UI::Drag("Min scale", settings.MinScale,
                                   {.Speed = 0.01f, .Min = 0.1f, .Max = 1.0f, .Format = "%.2f"});
                edited |= UI::Drag("Max scale", settings.MaxScale,
                                   {.Speed = 0.01f, .Min = 0.25f, .Max = 2.0f, .Format = "%.2f"});
                edited |= UI::Drag("Headroom", settings.Headroom,
                                   {.Speed = 0.01f, .Min = 0.0f, .Max = 0.5f, .Format = "%.2f"});
                edited |= UI::Drag("Max step", settings.MaxStep,
                                   {.Speed = 0.01f, .Min = 0.01f, .Max = 1.0f, .Format = "%.2f"});
                if (edited && viewport.IsDynamicResolutionEnabled())
                {
                    viewport.SetDynamicResolution(settings);
                }
            }

            // Render scale is a per-viewport property. While dynamic resolution is on it reads out
            // the live sub-rect scale; touching it is the manual override — it drops dynamic
            // resolution and holds the value, sizing the render target directly. Steps by 0.05 from
            // a 0.25 floor.
            f32 renderScale = viewport.GetRenderScale();
            if (UI::Drag("Render scale", renderScale,
                         {.Speed = 0.05f, .Min = 0.25f, .Max = 2.0f, .Format = "%.2f"}))
            {
                renderScale = glm::clamp(glm::round(renderScale / 0.05f) * 0.05f, 0.25f, 2.0f);
                if (viewport.IsDynamicResolutionEnabled())
                {
                    viewport.ClearDynamicResolution();
                }
                viewport.SetRenderScale(renderScale);
            }

            // The fixed allocation the sub-rect renders inside — read-only facts; the allocation
            // moves only on a region/window change or an explicit scale edit, never on frame-time
            // pressure.
            const uvec2 allocExtent = viewport.GetAllocationExtent();
            UI::Text(fmt::format("Allocation: {:.2f} ({}x{})", viewport.GetAllocationScale(),
                                 allocExtent.x, allocExtent.y));
        }
    }

    bool DebugViewCombo(Renderer::DebugView& mode)
    {
        i32 index = static_cast<i32>(mode);
        if (UI::Combo("View", index, Renderer::DebugViewNames))
        {
            mode = static_cast<Renderer::DebugView>(index);
            return true;
        }
        return false;
    }

    bool RenderSettingsEditor(SceneRendererSettings& settings, Renderer::ViewState& view,
                              Viewport& viewport)
    {
        const Renderer::Context& context =
            viewport.GetRenderer().GetOutput()->GetImage()->GetContext();
        const Renderer::SceneRenderer& renderer = viewport.GetRenderer();

        // Accumulates whether any topology field changed; the caller reconfigures on a true return.
        bool changed = false;

        changed |= DebugViewCombo(settings.Mode);

        if (auto section = UI::CollapsingHeader("Lighting & effects", TreeFlags::DefaultOpen))
        {
            changed |= UI::Checkbox("SSAO", settings.AO);
            changed |= UI::Checkbox("TAA", settings.TAA);
        }

        if (auto section = UI::CollapsingHeader("Bloom", TreeFlags::DefaultOpen))
        {
            // On/off and the kernel are topology; threshold/intensity/radius are per-frame
            // ViewState values. The per-bloom knobs grey out when bloom is off.
            changed |= UI::Checkbox("Enabled##bloom", settings.Bloom);
            auto bloomDisabled = UI::Disabled(!settings.Bloom);

            static constexpr std::array<string_view, 2> kernelNames{"COD (13-tap/tent)",
                                                                    "Dual Kawase"};
            i32 kernel = static_cast<i32>(settings.Kernel);
            if (UI::Combo("Kernel", kernel, kernelNames))
            {
                settings.Kernel = static_cast<Renderer::BloomKernel>(kernel);
                changed = true;
            }

            (void)UI::Drag("Threshold##bloom", view.BloomThreshold,
                           {.Speed = 0.01f, .Min = 0.0f, .Max = 8.0f});
            (void)UI::Drag("Intensity##bloom", view.BloomIntensity,
                           {.Speed = 0.01f, .Min = 0.0f, .Max = 4.0f});
            (void)UI::Drag("Radius##bloom", view.BloomRadius,
                           {.Speed = 0.01f, .Min = 0.0f, .Max = 4.0f});
        }

        if (auto section = UI::CollapsingHeader("Screen-space reflections"))
        {
            // The toggle and the trace resolution are topology; the intensity/distance/thickness/
            // roughness knobs are per-frame ViewState values. Everything greys out when SSR is off.
            changed |= UI::Checkbox("Enabled##ssr", settings.SSR);
            auto ssrDisabled = UI::Disabled(!settings.SSR);

            static constexpr std::array<string_view, 3> ssrResolutionNames{"Full", "Half",
                                                                           "Quarter"};
            i32 ssrResolution = static_cast<i32>(settings.SsrResolutionScale);
            if (UI::Combo("Trace resolution", ssrResolution, ssrResolutionNames))
            {
                settings.SsrResolutionScale =
                    static_cast<SceneRendererSettings::SsrResolution>(ssrResolution);
                changed = true;
            }

            (void)UI::Drag("Intensity##ssr", view.SsrIntensity,
                           {.Speed = 0.01f, .Min = 0.0f, .Max = 4.0f});
            (void)UI::Drag("Max distance##ssr", view.SsrMaxDistance,
                           {.Speed = 0.1f, .Min = 0.0f, .Max = 200.0f});
            (void)UI::Drag("Thickness##ssr", view.SsrThickness,
                           {.Speed = 0.01f, .Min = 0.0f, .Max = 4.0f});
            (void)UI::Slider("Max roughness##ssr", view.SsrMaxRoughness,
                             {.Min = 0.0f, .Max = 1.0f});
        }

        if (auto section = UI::CollapsingHeader("Depth of field"))
        {
            // The toggle is topology; focus/aperture and the two quality knobs are per-frame
            // ViewState values. CoC scale has no widget at all — the viewport glue always derives
            // it from the target's pixel height and the camera's sensor.
            changed |= UI::Checkbox("Enabled##dof", settings.DepthOfField);
            auto dofDisabled = UI::Disabled(!settings.DepthOfField);

            // A Physical camera authors the lens fields on every push, so editing them here would
            // write values nothing consults; the quality knobs stay live in every camera mode.
            const bool lensFromCamera = view.DofFromPhysicalCamera;
            {
                auto lensDisabled = UI::Disabled(lensFromCamera);
                (void)UI::Drag("Focus distance##dof", view.DofFocusDistance,
                               {.Speed = 0.05f, .Min = 0.01f, .Max = 1000.0f});
                (void)UI::Drag("Aperture##dof", view.DofAperture,
                               {.Speed = 0.0005f, .Min = 0.0f, .Max = 0.5f, .Format = "%.4f m"});
            }
            if (lensFromCamera)
            {
                UI::TextDisabled(DofPhysicalCameraNote);
            }

            (void)UI::Drag("Max blur radius##dof", view.DofMaxCoc,
                           {.Speed = 0.25f, .Min = 0.0f, .Max = Renderer::DofCocCeiling});
            i32 rings = static_cast<i32>(view.DofRingCount);
            if (UI::Slider("Rings##dof", rings, 1, static_cast<i32>(Renderer::MaxDofRings)))
            {
                view.DofRingCount = static_cast<u32>(rings);
            }
        }

        if (auto section = UI::CollapsingHeader("Shadows", TreeFlags::DefaultOpen))
        {
            changed |= UI::Checkbox("Directional shadows", settings.Shadows);
            {
                auto shadowsDisabled = UI::Disabled(!settings.Shadows);

                i32 cascadeCount = static_cast<i32>(settings.CascadeCount);
                if (UI::Slider("Cascades##count", cascadeCount, 1,
                               static_cast<i32>(Renderer::MaxCascades)))
                {
                    settings.CascadeCount = static_cast<u32>(cascadeCount);
                    changed = true;
                }

                i32 shadowResolution = static_cast<i32>(settings.ShadowResolution);
                if (UI::Drag("Resolution##shadow", shadowResolution,
                             {.Speed = 16.0f,
                              .Min = 256.0f,
                              .Max = static_cast<f32>(renderer.GetMaxShadowResolution())}))
                {
                    settings.ShadowResolution = static_cast<u32>(shadowResolution);
                    changed = true;
                }

                if (UI::Slider("Split lambda", settings.CascadeSplitLambda,
                               {.Min = 0.0f, .Max = 1.0f}))
                {
                    changed = true;
                }

                if (UI::Drag("Max distance##shadow", settings.MaxShadowDistance,
                             {.Speed = 1.0f, .Min = 0.0f, .Max = 10000.0f}))
                {
                    changed = true;
                }
            }

            changed |= UI::Checkbox("Punctual shadows", settings.PunctualShadows);
            {
                auto punctualDisabled = UI::Disabled(!settings.PunctualShadows);

                i32 punctualResolution = static_cast<i32>(settings.PunctualShadowResolution);
                if (UI::Drag("Resolution##punctual", punctualResolution,
                             {.Speed = 16.0f,
                              .Min = 256.0f,
                              .Max = static_cast<f32>(renderer.GetMaxPunctualShadowResolution())}))
                {
                    settings.PunctualShadowResolution = static_cast<u32>(punctualResolution);
                    changed = true;
                }
            }
        }

        if (auto section = UI::CollapsingHeader("Exposure", TreeFlags::DefaultOpen))
        {
            // Exposure and the auto-exposure tuning are per-frame ViewState values; the metering
            // toggle is topology (it inserts the histogram compute pass). With metering on,
            // Exposure biases the adapted value; the tuning knobs grey out with it off. The drag's
            // id is suffixed apart from the section header's (CollapsingHeader pushes no id scope).
            (void)UI::Drag("Exposure##value", view.Exposure,
                           {.Speed = 0.01f, .Min = 0.0f, .Max = 16.0f});
            changed |= UI::Checkbox("Auto exposure", settings.AutoExposure);
            auto meteringDisabled = UI::Disabled(!settings.AutoExposure);
            (void)UI::Drag("Key", view.AutoExposureKey,
                           {.Speed = 0.005f, .Min = 0.01f, .Max = 1.0f});
            (void)UI::Drag("Min luminance", view.AutoExposureMinLuminance,
                           {.Speed = 0.001f, .Min = 0.0f, .Max = 8.0f, .Format = "%.4f"});
            (void)UI::Drag("Max luminance", view.AutoExposureMaxLuminance,
                           {.Speed = 1.0f, .Min = 0.01f, .Max = 10000.0f});
            (void)UI::Drag("Adaptation speed", view.AutoExposureSpeed,
                           {.Speed = 0.05f, .Min = 0.0f, .Max = 20.0f});
        }

        // The sky is the scene's Sky component, resolved by the renderer itself — not a topology
        // toggle or a per-frame ViewState value — so it is authored and edited in the inspector,
        // not here.

        if (auto section = UI::CollapsingHeader("Culling"))
        {
            changed |= UI::Checkbox("Frustum culling", settings.FrustumCull);

            // The GPU arm is a different pass topology, so the selector and the occlusion toggle
            // both drive a recompile. The selector greys out where the device cannot honor GPU
            // (it would silently degrade to CPU); the stats panel shows the active path.
            {
                auto gpuUnsupported = UI::Disabled(!context.IsGpuDrivenCullingSupported());
                static constexpr std::array<string_view, 2> cullNames{"CPU", "GPU"};
                i32 cull = static_cast<i32>(settings.Cull);
                if (UI::Combo("Cull mode", cull, cullNames))
                {
                    settings.Cull = static_cast<SceneRendererSettings::CullMode>(cull);
                    changed = true;
                }

                auto cpuPath = UI::Disabled(settings.Cull != SceneRendererSettings::CullMode::GPU);
                changed |= UI::Checkbox("GPU occlusion", settings.Occlusion);
            }
        }

        if (auto section = UI::CollapsingHeader("Resolution"))
        {
            // Adaptive resolution and the manual render-scale override drive the viewport
            // imperatively — they recreate or resize renderer resources directly, not through the
            // Configure recompile this function reports.
            DrawResolutionControls(viewport, context);
        }

        if (auto section = UI::CollapsingHeader("Authoring"))
        {
            // The immediate-mode debug-draw flush pass and the entity-id picking pass, both off
            // by default — authoring aids a consumer enables for a viewport's lifetime.
            changed |= UI::Checkbox("Debug draw", settings.DebugDraw);
            changed |= UI::Checkbox("Picking", settings.Picking);
        }

        return changed;
    }
}
