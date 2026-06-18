#include <Veng/UI/Query.h>

#include <imgui.h>

namespace Veng::UI
{
    bool ItemHovered()
    {
        return ImGui::IsItemHovered();
    }

    bool ItemEdited()
    {
        return ImGui::IsItemDeactivatedAfterEdit();
    }

    void Tooltip(string_view text)
    {
        if (ImGui::IsItemHovered())
            ImGui::SetTooltip("%.*s", static_cast<int>(text.size()), text.data());
    }

    f32 FrameRate()
    {
        return ImGui::GetIO().Framerate;
    }
}
