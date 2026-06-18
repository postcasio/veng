#include <Veng/UI/Layout.h>

#include <imgui.h>

namespace Veng::UI
{
    void Separator()
    {
        ImGui::Separator();
    }

    void SameLine()
    {
        ImGui::SameLine();
    }

    void Spacing()
    {
        ImGui::Spacing();
    }

    vec2 ContentRegionAvail()
    {
        const ImVec2 avail = ImGui::GetContentRegionAvail();
        return vec2(avail.x, avail.y);
    }

    void ScrollToHere()
    {
        ImGui::SetScrollHereY(1.0f);
    }
}
