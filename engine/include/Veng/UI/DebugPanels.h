#pragma once
#include <Veng/Veng.h>

#include <array>

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

    /// @brief A stateful rolling frame-time graph owning its GPU-frame-time history ring.
    ///
    /// The one stateful `Veng::UI` helper: a small value type the caller persists across
    /// frames (a panel-local member), holding a fixed-capacity ring of millisecond samples.
    /// Push a sample each frame and call `Draw` (or `Draw(viewport)`, which samples
    /// `Context::GetLastGpuFrameTimeMs()` itself) to plot the history with a min/avg/max
    /// readout. Imgui-free in its surface, consistent with the stateless widget wrappers.
    class FrameTimeGraph
    {
    public:
        /// @brief Constructs an empty graph with a zeroed history ring.
        FrameTimeGraph() = default;

        /// @brief Appends one GPU frame-time sample, overwriting the oldest once full.
        /// @param milliseconds  The frame's GPU time in milliseconds.
        void Push(f32 milliseconds);

        /// @brief Samples the viewport's renderer GPU frame time, then plots the history.
        ///
        /// Reads `Context::GetLastGpuFrameTimeMs()` through the viewport's render context and
        /// pushes it before plotting, so a caller need not sample the timeline itself. Draws a
        /// disabled note instead of a plot when the device exposes no timestamp queries
        /// (`Context::IsGpuTimingSupported()` is false). Draws into the current window.
        /// @param viewport  The viewport whose render context the GPU frame time is read from.
        void Draw(const Renderer::Viewport& viewport);

        /// @brief Plots the accumulated history with a min/avg/max readout above the line graph.
        ///
        /// The axis is pinned to `[0, max·1.25]` (floored at one 60 Hz frame) so a line's
        /// height reads as absolute milliseconds. Draws into the current window. Use the
        /// `Draw(viewport)` overload to sample the GPU frame time automatically.
        void Draw();

    private:
        /// @brief Fixed sample capacity of the history ring.
        static constexpr usize Capacity = 240;

        /// @brief The rolling millisecond samples; filled from index 0 until it wraps.
        std::array<f32, Capacity> m_Samples{};
        /// @brief The next write slot (and, once full, the oldest sample).
        usize m_Head = 0;
        /// @brief The number of valid samples, saturating at Capacity.
        usize m_Count = 0;
    };

    /// @brief Draws the renderer's topology toggles and per-frame view knobs in one panel.
    ///
    /// Edits the `SceneRendererSettings` topology knobs (the debug-view mode, the SSAO / TAA /
    /// SSR / shadow / punctual-shadow / skybox / bloom / frustum-cull / GPU-occlusion / debug-draw
    /// toggles, the cascade / shadow-resolution / SSR-resolution / cull-mode controls) and the
    /// per-frame `ViewState` knobs (exposure, environment intensity, bloom threshold / intensity /
    /// radius). The viewport's adaptive-resolution controls (dynamic resolution, the allocation-tier
    /// outer loop, and the manual render-scale override) drive the viewport imperatively, since they
    /// recreate or resize renderer resources directly.
    ///
    /// Returns whether a topology field of `settings` changed this frame, per the editable-widget
    /// idiom. The helper does **not** reconfigure: the caller decides whether to call
    /// `Viewport::Configure(settings)` (a recompile) on a true return — the engine reserves
    /// `Configure` for the resource owner. The per-frame `ViewState` edits ride the next frame's
    /// push with no reconfigure.
    /// @param settings  The renderer topology/sizing knobs to edit in place.
    /// @param view      The per-frame view knobs (exposure, environment, bloom) to edit in place.
    /// @param viewport  The viewport whose adaptive-resolution controls this panel drives.
    /// @return True the frame a `settings` topology field changed (the caller should `Configure`).
    [[nodiscard]] bool RenderSettingsEditor(Renderer::SceneRendererSettings& settings,
                                            Renderer::ViewState& view,
                                            Renderer::Viewport& viewport);
}
