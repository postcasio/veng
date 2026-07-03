#include "Joined.h"

namespace Veng::UI
{
    namespace
    {
        // veng is single-threaded; no synchronization is provided. A joined group is a
        // strictly-nested draw scope, so a single active flag plus an item counter is enough.
        bool g_Active = false;
        i32 g_ItemIndex = 0;
    }

    void JoinedBegin(string_view id)
    {
        const string label(id);
        ImGui::PushID(label.c_str());
        g_Active = true;
        g_ItemIndex = 0;

        // No inter-item spacing so the segments touch, and square frames on stock widgets: the
        // theme draws frame borders, where patching a rounded border corner would glitch, so the
        // artifact-free treatment is to flatten stock rounding for the group's duration. Custom
        // widgets keep true per-corner rounding on their outer edges via JoinedCornerFlags.
        ImGui::PushStyleVar(ImGuiStyleVar_ItemSpacing,
                            ImVec2(0.0f, ImGui::GetStyle().ItemSpacing.y));
        ImGui::PushStyleVar(ImGuiStyleVar_FrameRounding, 0.0f);
    }

    void JoinedEnd()
    {
        ImGui::PopStyleVar(2);
        ImGui::PopID();
        g_Active = false;
        g_ItemIndex = 0;
    }

    bool JoinedActive()
    {
        return g_Active;
    }

    void JoinedPreItem()
    {
        if (g_Active && g_ItemIndex > 0)
        {
            ImGui::SameLine(0.0f, 0.0f);
        }
    }

    void JoinedPostItem()
    {
        if (g_Active)
        {
            ++g_ItemIndex;
        }
    }

    ImDrawFlags JoinedCornerFlags(bool roundLeft, bool roundRight)
    {
        // Inside a group every widget squares: stock frames are already flattened by the pushed
        // zero FrameRounding, and a custom widget cannot know whether it is the group's last
        // item, so rounding an edge here would leave a rounded seam mid-group or an asymmetric
        // outer corner beside a squared stock widget. Uniform square is the artifact-free shape.
        if (g_Active)
        {
            return ImDrawFlags_RoundCornersNone;
        }

        ImDrawFlags flags = ImDrawFlags_RoundCornersNone;
        if (roundLeft)
        {
            flags |= ImDrawFlags_RoundCornersLeft;
        }
        if (roundRight)
        {
            flags |= ImDrawFlags_RoundCornersRight;
        }
        return flags;
    }
}
