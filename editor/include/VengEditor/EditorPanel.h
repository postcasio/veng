#pragma once

#include <Veng/Veng.h>
#include <imgui.h>

namespace VengEditor
{
    // A dockable editor window. The host owns the open/close toggle, the dock id,
    // and the Window-menu wiring; a panel carries only its title and the body it
    // draws each frame inside the host-managed ImGui::Begin/End.
    class EditorPanel
    {
    public:
        virtual ~EditorPanel() = default;

        [[nodiscard]] virtual Veng::string_view GetTitle() const = 0;
        [[nodiscard]] virtual ImGuiWindowFlags GetWindowFlags() const { return 0; }
        virtual void OnImGui() = 0;
    };
}
