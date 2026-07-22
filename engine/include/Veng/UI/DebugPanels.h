#pragma once
#include <Veng/Veng.h>

#include <array>
#include <span>
#include <string>

/// @brief À-la-carte renderer debug panels built on the `Veng::UI` widget vocabulary.
///
/// Composable helpers a game arranges in its own windows — the renderer stats read-out, the
/// compact performance panel over the profiler's live scope data, and the render-settings
/// editor — rather than one turnkey overlay. Each is authored against `Veng::UI` and imgui-free
/// in its signatures, naming only engine renderer types. None opens its own window: a caller
/// wraps the panel it wants in a `UI::Window`/`UI::Child`, skips one in a shipping build, or
/// embeds it in a larger profiler.

namespace Veng::Renderer
{
    class Viewport;
    struct SceneRendererSettings;
    struct ViewState;
    enum class DebugView : u8;
}

namespace Veng::UI
{
    /// @brief Draws the viewport renderer's read-only diagnostic stats as text rows.
    ///
    /// Reads the viewport and its `SceneRenderer` for the live allocation tier (index +
    /// scale + auto/static), the render scale and target extent, the cull funnel (gathered
    /// → frustum-survived → drawn, plus the GPU occlusion survivor count under the GPU cull
    /// path), the active cull mode, and the broadphase rebuild state + node count. A pure
    /// read-out — it draws text only and edits nothing, so it returns void. Draws into the
    /// current window; a caller wraps it in its own `UI::Window`.
    /// @param viewport  The viewport whose renderer the stats are read from.
    void RendererStatsPanel(const Renderer::Viewport& viewport);

    /// @brief A fixed-capacity rolling ring of millisecond samples for the panel's history charts.
    ///
    /// Samples are appended head-first and the ring saturates at `Capacity`, overwriting the oldest
    /// once full; `PlotOffset()` reports the oldest sample's index so a `UI::PlotLines` draws it
    /// oldest-to-newest without a rotation. A plain value type with no imgui dependency.
    struct PerformanceHistory
    {
        /// @brief Fixed sample capacity of the ring.
        static constexpr usize Capacity = 240;

        /// @brief The rolling millisecond samples; filled from index 0 until it wraps.
        std::array<f32, Capacity> Samples{};
        /// @brief The next write slot (and, once full, the oldest sample).
        usize Head = 0;
        /// @brief The number of valid samples, saturating at `Capacity`.
        usize Count = 0;

        /// @brief Appends one sample, overwriting the oldest once full.
        /// @param milliseconds  The sample value in milliseconds.
        void Push(f32 milliseconds);

        /// @brief Returns the most recently pushed sample, or 0 before the first push.
        [[nodiscard]] f32 Last() const;

        /// @brief Returns the plot ring offset: the write head once the ring has wrapped, else 0.
        [[nodiscard]] i32 PlotOffset() const;
    };

    /// @brief One candidate frame phase for the heaviest-N selection: its name and inclusive cost.
    struct PhaseSample
    {
        /// @brief The phase scope's name (a stable `Frame/*`-family string).
        string Name;
        /// @brief The phase's last-frame inclusive time, in milliseconds.
        f64 InclusiveMs = 0.0;
    };

    /// @brief One GPU pass band — a display name and its millisecond cost.
    ///
    /// Both the input and the output of `FoldGpuBands`: the fold takes the frame's grouped passes
    /// and returns at most a fixed number of bands, the surplus summed into a trailing "Other".
    struct GpuBand
    {
        /// @brief The pass (or folded group) display name.
        string Name;
        /// @brief The band's GPU cost, in milliseconds.
        f32 Milliseconds = 0.0f;
    };

    /// @brief One row of the scope table: the numeric columns for a scope active last frame.
    struct ScopeRow
    {
        /// @brief The scope's name.
        string Name;
        /// @brief Inclusive time (self plus children), in milliseconds.
        f64 InclusiveMs = 0.0;
        /// @brief Self time (inclusive minus nested children), in milliseconds.
        f64 SelfMs = 0.0;
        /// @brief Inclusive time as a fraction of the frame, in percent.
        f64 PercentOfFrame = 0.0;
        /// @brief Times the scope was entered in the reported frame.
        u64 CallCount = 0;
    };

    /// @brief The scope table's sort column.
    enum class ScopeSortColumn : u8
    {
        /// @brief Sort by scope name (lexicographic).
        Name,
        /// @brief Sort by inclusive time.
        Inclusive,
        /// @brief Sort by self time.
        Self,
        /// @brief Sort by percent of frame.
        Percent,
        /// @brief Sort by call count.
        Calls,
    };

    /// @brief Selects the heaviest phases to plot, with hysteresis so a near-tie does not flicker.
    ///
    /// Keeps `selection` (the phases currently shown, at most `count`) stable across frames: an
    /// incumbent holds its slot while its cost stays within `hysteresis` of the `count`-th heaviest
    /// candidate, and only a challenger that clears that margin displaces it. Remaining slots fill
    /// heaviest-first from the candidates. Ties break by name, so the result is deterministic. The
    /// candidate list may be in any order; `selection` is rewritten in place with the new set, in
    /// display order (surviving incumbents first, then fresh entrants heaviest-first).
    /// @param candidates  The frame's candidate phases (name + inclusive ms).
    /// @param selection   The persisted selection, read and rewritten in place.
    /// @param count       The maximum number of phases to select.
    /// @param hysteresis  The relative margin (e.g. 0.25) a challenger must clear to displace an
    ///                    incumbent.
    void SelectHeaviestPhases(std::span<const PhaseSample> candidates, vector<string>& selection,
                              usize count, f32 hysteresis);

    /// @brief Folds the frame's GPU passes into at most `maxBands` bands, the surplus summed as "Other".
    ///
    /// Sorts the passes by cost (descending, ties by name) and returns them unchanged when they fit
    /// within `maxBands`; otherwise returns the heaviest `maxBands - 1` followed by a single "Other"
    /// band holding the sum of the rest. The total across the returned bands always equals the total
    /// across the input, so the stack it draws neither gains nor loses time — the bound on band count
    /// never bounds the reported total.
    /// @param passes    The frame's grouped GPU passes.
    /// @param maxBands  The maximum number of bands to return (must be at least 1).
    /// @return The bounded band list, heaviest-first, with a trailing "Other" when folded.
    [[nodiscard]] vector<GpuBand> FoldGpuBands(std::span<const GpuBand> passes, usize maxBands);

    /// @brief Sorts the scope table's rows by one column, ascending or descending.
    ///
    /// Every non-name column breaks ties by name so the order is total and stable frame to frame;
    /// the name column breaks ties on itself (already total). Sorting is the panel's only per-frame
    /// reshaping of the aggregate snapshot — a new scope is a new row and changes nothing else.
    /// @param rows       The rows to sort in place.
    /// @param column     The column to order by.
    /// @param ascending  True for ascending order, false for descending.
    void SortScopeRows(vector<ScopeRow>& rows, ScopeSortColumn column, bool ascending);

    /// @brief Returns the notice the panel draws in place of its profiler-sourced bands, or empty.
    ///
    /// The phase series and the scope table are populated only when the profiler is compiled in
    /// (`VE_PROFILE=ON`) and an instance is installed. When either is absent this returns a stated
    /// line explaining why — never empty, so the panel never degrades to a silent empty table that
    /// reads as "nothing is running". When the data is available it returns an empty view.
    /// @return The disabled-notice line, or an empty view when per-scope data is available.
    [[nodiscard]] string_view PerformanceProfilerNotice();

    /// @brief A compact, organized performance panel over the profiler's live per-frame scope data.
    ///
    /// Drawn windowless in five bands top to bottom (the caller owns the window, as
    /// `RendererStatsPanel` does): a **budget bar** (stacked CPU/GPU against the frame target, FPS,
    /// frame index), a **frame-history chart** (CPU total, GPU total, and the heaviest few phases —
    /// a series count fixed by construction so the legend never grows), a **GPU pass breakdown**
    /// (a bounded stack, the surplus passes folded into "Other"), a **sortable scope table**, and a
    /// **counter strip** (the renderer's cull-funnel counters, current frame, no history).
    ///
    /// The table is the design's scalability property: it draws one row per scope active in the last
    /// completed frame — name, inclusive ms, self ms, % of frame, call count — read from the
    /// profiler's aggregate snapshot each frame, not mirrored. A subsystem that adds a scope gets a
    /// table row for free and changes nothing about the panel's layout, its two fixed-series charts,
    /// their legends, or its cost. The panel's own state is just the rolling chart history and the
    /// table's sort key.
    ///
    /// Both the whole-frame CPU delta (from `Time`) and the GPU timings (from `Context`) are read
    /// without the profiler, so the budget bar, the GPU pass chart, and the frame-history CPU/GPU
    /// totals draw regardless of build configuration. Under `VE_PROFILE=OFF` — or with no profiler
    /// installed — the phase series and the scope table are replaced by a single stated line
    /// (`PerformanceProfilerNotice`); the panel always builds and always draws.
    class PerformancePanel
    {
    public:
        /// @brief Constructs an empty panel with zeroed history rings and no selection.
        PerformancePanel() = default;

        /// @brief Samples this frame's timings and scope aggregates, then draws the five bands.
        ///
        /// Reads the CPU frame delta from `Time`, the GPU whole-frame and per-pass timings through
        /// the viewport's render context, the per-scope aggregates from the active profiler (when
        /// installed), and the renderer's cull-funnel counters from the viewport's `SceneRenderer`.
        /// Draws into the current window.
        /// @param viewport  The viewport whose render context and renderer the timings are read from.
        void Draw(const Renderer::Viewport& viewport);

    private:
        /// @brief The maximum number of frame phases plotted on the frame-history chart.
        static constexpr usize PhaseSlots = 3;
        /// @brief The maximum number of bands on the GPU pass breakdown (last is "Other" when folded).
        static constexpr usize GpuBandSlots = 6;
        /// @brief The relative margin a phase must clear to displace an incumbent (see SelectHeaviestPhases).
        static constexpr f32 PhaseHysteresis = 0.25f;

        /// @brief The CPU whole-frame history, a fixed series on the frame-history chart.
        PerformanceHistory m_Cpu;
        /// @brief The whole-frame GPU history, a fixed series on the frame-history chart.
        PerformanceHistory m_Gpu;
        /// @brief Per-slot phase histories; each slot follows the phase currently occupying it.
        std::array<PerformanceHistory, PhaseSlots> m_PhaseSlots;
        /// @brief The phases currently selected for the slots, in display order (hysteresis state).
        vector<string> m_PhaseNames;
        /// @brief Per-slot GPU-band histories; each slot follows the band currently occupying it.
        std::array<PerformanceHistory, GpuBandSlots> m_GpuSlots;
        /// @brief The GPU bands currently occupying the slots, in display order.
        vector<string> m_GpuBandNames;
        /// @brief The scope table's active sort column.
        ScopeSortColumn m_SortColumn = ScopeSortColumn::Inclusive;
        /// @brief Whether the scope table sorts ascending (false = descending).
        bool m_SortAscending = false;
        /// @brief The scope table's name filter text.
        string m_Filter;
    };

    /// @brief Draws the renderer debug-view selector as a combo over every DebugView arm.
    ///
    /// The reusable "View" dropdown `RenderSettingsEditor` embeds — equally placeable on its
    /// own (a toolbar, a viewport header). Selecting an arm is a topology change: on a true
    /// return the caller reconfigures the renderer with the edited settings, exactly as for
    /// `RenderSettingsEditor`.
    /// @param mode  The debug-view arm edited in place.
    /// @return True the frame the selection changed (the caller should `Configure`).
    [[nodiscard]] bool DebugViewCombo(Renderer::DebugView& mode);

    /// @brief Draws the renderer's full settings surface: every topology toggle and per-frame
    /// view knob, grouped into collapsible sections.
    ///
    /// Edits every exposable `SceneRendererSettings` topology knob (the debug-view mode; the
    /// SSAO / TAA / SSR / emissive / shadow / punctual-shadow / bloom / auto-exposure /
    /// frustum-cull / GPU-occlusion / debug-draw / picking toggles; the cascade,
    /// shadow-resolution, SSR-resolution, bloom-kernel, and cull-mode controls) and the
    /// per-frame `ViewState` knobs (exposure and the auto-exposure key/clamps/speed, the bloom
    /// threshold / intensity / radius, the SSR intensity / distance / thickness / roughness
    /// cutoff). A knob whose feature is off, or that the device cannot honor (GPU cull without
    /// `IsGpuDrivenCullingSupported`, dynamic resolution without timestamps), draws disabled.
    /// The viewport's adaptive-resolution controls (dynamic resolution and the manual
    /// render-scale override) drive the viewport imperatively, since they recreate or resize
    /// renderer resources directly. The sky is the scene's `Sky` component, resolved by the
    /// renderer itself — authored in the inspector, not here.
    ///
    /// Returns whether a topology field of `settings` changed this frame, per the editable-widget
    /// idiom. The helper does **not** reconfigure: the caller decides whether to call
    /// `Viewport::Configure(settings)` (a recompile) on a true return — the engine reserves
    /// `Configure` for the resource owner. The per-frame `ViewState` edits ride the next frame's
    /// push with no reconfigure.
    /// @param settings  The renderer topology/sizing knobs to edit in place.
    /// @param view      The per-frame view knobs (exposure/auto-exposure, bloom, SSR) to edit in
    ///                  place.
    /// @param viewport  The viewport whose adaptive-resolution controls this panel drives.
    /// @return True the frame a `settings` topology field changed (the caller should `Configure`).
    [[nodiscard]] bool RenderSettingsEditor(Renderer::SceneRendererSettings& settings,
                                            Renderer::ViewState& view,
                                            Renderer::Viewport& viewport);
}
