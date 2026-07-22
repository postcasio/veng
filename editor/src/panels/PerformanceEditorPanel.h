#pragma once

#include <VengEditor/EditorPanel.h>

#include <Veng/UI/DebugPanels.h>

namespace VengEditor
{
    class EditorHost;

    /// @brief The editor's performance panel — hosts the engine `PerformancePanel` over a scene viewport.
    ///
    /// The editor's first performance surface. It owns no viewport and records no render: it reads.
    /// Each frame `OnUI` wraps `Veng::UI::PerformancePanel` over a scene viewport resolved from the
    /// host — the focused asset-editor document's scene viewport when one is focused, otherwise the
    /// first open scene viewport.
    ///
    /// **Which viewport it reports when several documents are open:** the focused document's (else
    /// the first open one). This ambiguity is deliberately visible rather than implied — but it is
    /// narrow: the budget bar, the frame-history CPU/GPU totals, the phase series, and the scope
    /// table are all process-global (from `Time`, `Context`, and the active profiler) and read
    /// identically whichever viewport is passed; only the counter strip's renderer cull-funnel
    /// figures are per-viewport and follow the resolved document. The editor registers no `Presented`
    /// viewport of its own, so there is no single "editor frame" viewport to report instead. With no
    /// scene document open the panel states so rather than drawing stale data.
    class PerformanceEditorPanel final : public EditorPanel
    {
    public:
        /// @brief Constructs the panel over the host it resolves the active scene viewport from.
        /// @param host  The editor host, borrowed for the panel's lifetime.
        explicit PerformanceEditorPanel(EditorHost& host) : m_Host(host) {}

        /// @brief Returns the panel's window title.
        [[nodiscard]] Veng::string_view GetTitle() const override { return "Performance"; }

        /// @brief Draws the engine performance panel over the resolved scene viewport.
        void OnUI() override;

    private:
        /// @brief The host the active scene viewport is resolved through each frame.
        EditorHost& m_Host;
        /// @brief The engine performance panel, owning its rolling history and sort key across frames.
        Veng::UI::PerformancePanel m_Panel;
    };
}
