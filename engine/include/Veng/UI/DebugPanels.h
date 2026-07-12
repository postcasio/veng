#pragma once
#include <Veng/Veng.h>

#include <array>
#include <map>
#include <string>

/// @brief À-la-carte renderer debug panels built on the `Veng::UI` widget vocabulary.
///
/// Three composable helpers a game arranges in its own windows — the renderer stats
/// read-out, a stateful frame-time graph, and the render-settings editor — rather than
/// one turnkey overlay. Each is authored against `Veng::UI` and imgui-free in its
/// signatures, naming only engine renderer types. None opens its own window: a caller
/// wraps the panel it wants in a `UI::Window`/`UI::Child`, skips one in a shipping build,
/// or embeds it in a larger profiler.

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

    /// @brief A stateful rolling frame-time graph combining the whole-frame and per-pass timings.
    ///
    /// The one stateful `Veng::UI` helper: a small value type the caller persists across frames
    /// (a panel-local member), owning rolling millisecond histories for the CPU frame delta, the
    /// whole-frame GPU time, and each render pass's GPU cost. One `Draw(viewport)` per frame
    /// samples all three from the viewport's render context (and the frame delta from `Time`),
    /// then plots the whole-frame GPU time and every grouped pass as colored lines on one shared
    /// auto-scaled chart with a color-swatch legend, and the CPU frame time on its own axis
    /// below. Imgui-free in its surface, consistent with the stateless widget wrappers.
    class FrameTimeGraph
    {
    public:
        /// @brief Constructs an empty graph with zeroed history rings.
        FrameTimeGraph() = default;

        /// @brief Samples this frame's CPU/GPU/per-pass timings, then plots the combined graph.
        ///
        /// Reads the whole-frame GPU time and the per-pass GPU breakdown
        /// (`Context::GetLastGpuFrameTimeMs()` / `Context::GetLastGpuPassTimings()`) through the
        /// viewport's render context and the CPU frame delta from `Time::GetDeltaTime()`, appends
        /// each to its rolling history, then draws them: the whole-frame GPU envelope (white) and
        /// every grouped pass (bloom / hi-Z mip sweeps collapsed) as colored lines on one shared
        /// auto-scaled chart, a two-column legend, and the CPU frame time below on its own axis.
        /// The GPU sections are replaced by a disabled note when the device exposes no timestamp
        /// queries (`Context::IsGpuTimingSupported()` is false). Draws into the current window.
        /// @param viewport  The viewport whose render context the timings are read from.
        void Draw(const Renderer::Viewport& viewport);

    private:
        /// @brief Fixed sample capacity of each history ring.
        static constexpr usize Capacity = 240;

        /// @brief A fixed-capacity rolling ring of millisecond samples.
        struct History
        {
            /// @brief The rolling millisecond samples; filled from index 0 until it wraps.
            std::array<f32, Capacity> Samples{};
            /// @brief The next write slot (and, once full, the oldest sample).
            usize Head = 0;
            /// @brief The number of valid samples, saturating at Capacity.
            usize Count = 0;

            /// @brief Appends one sample, overwriting the oldest once full.
            /// @param milliseconds  The sample value in milliseconds.
            void Push(f32 milliseconds);

            /// @brief Returns the most recently pushed sample, or 0 before the first push.
            [[nodiscard]] f32 Last() const;

            /// @brief Returns the plot ring offset: the write head once wrapped, else 0.
            [[nodiscard]] i32 PlotOffset() const;
        };

        /// @brief The CPU frame-delta history, plotted on its own axis below the GPU chart.
        History m_Cpu;
        /// @brief The whole-frame GPU-time history, the white envelope line on the shared chart.
        History m_Gpu;
        /// @brief Per-pass GPU-cost histories, keyed by grouped pass name; a pass absent this
        /// frame simply stops receiving samples until it returns.
        map<string, History> m_Passes;
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
