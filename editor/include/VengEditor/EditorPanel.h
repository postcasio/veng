#pragma once

#include <Veng/Veng.h>
#include <Veng/UI/Types.h>

namespace Veng::Renderer
{
    class CommandBuffer;
}

namespace VengEditor
{
    /// @brief Base class for a dockable editor window.
    ///
    /// The host owns the open/close toggle, the dock id, and the Window-menu
    /// wiring. A panel provides its title and draws its body each frame inside
    /// the host-managed UI::Window scope.
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
        virtual void OnImGui() = 0;

        /// @brief Records this frame's offscreen render into cmd.
        ///
        /// Called on every open panel before the ImGui frame is built, so the
        /// output is sampleable when OnImGui draws it. Default is a no-op;
        /// only render-owning panels (e.g. scene viewport, material preview) override it.
        /// @param cmd Command buffer for the current frame.
        virtual void OnRender(Veng::Renderer::CommandBuffer& cmd) {}
    };
}
