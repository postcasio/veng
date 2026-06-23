#pragma once

#include <Veng/Veng.h>
#include <Veng/UI/Types.h>

namespace VengEditor
{
    /// @brief Base class for a dockable editor window.
    ///
    /// The host owns the open/close toggle, the dock id, and the Window-menu
    /// wiring. A panel provides its title and draws its body each frame inside
    /// the host-managed UI::Window scope. A render-owning panel holds a registered
    /// Veng::Renderer::Viewport; the engine drive-list renders it each frame, so the
    /// panel records no scene render of its own.
    class EditorPanel
    {
    public:
        virtual ~EditorPanel() = default;

        /// @brief Returns the panel's window title (stable across frames).
        [[nodiscard]] virtual Veng::string_view GetTitle() const = 0;

        /// @brief Returns additional ImGui window flags for this panel.
        [[nodiscard]] virtual Veng::UI::WindowFlags GetWindowFlags() const
        {
            return Veng::UI::WindowFlags::None;
        }

        /// @brief Draws the panel's ImGui contents into the current frame.
        virtual void OnUI() = 0;

        /// @brief Submits this panel's top-level window(s) for the frame.
        ///
        /// The host calls this once per open panel. The default wraps OnUI in a
        /// single UI::Window titled GetTitle() (with the panel's GetWindowFlags() and
        /// edge-to-edge padding for a NoScrollbar panel). An asset editor that hosts a
        /// private dockspace overrides this to submit its document window and the
        /// class-tagged child windows that dock into it.
        /// @param open  Host-owned visibility flag, toggled by the window close button.
        virtual void Draw(bool* open);
    };
}
