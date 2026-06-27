#include <Veng/UI/Layout.h>

#include <imgui.h>

#include <cfloat>

namespace Veng::UI
{
    namespace
    {
        // ImGui takes const char*, not string_view; materialize at the call boundary.
        string AsCStr(string_view s)
        {
            return string(s);
        }
    }

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
        return ImGui::GetContentRegionAvail();
    }

    vec2 CursorPos()
    {
        return ImGui::GetCursorPos();
    }

    void SetCursorPos(vec2 pos)
    {
        ImGui::SetCursorPos(pos);
    }

    void ScrollToHere()
    {
        ImGui::SetScrollHereY(1.0f);
    }

    void Indent()
    {
        ImGui::Indent();
    }

    void Unindent()
    {
        ImGui::Unindent();
    }

    void Dummy(vec2 size)
    {
        ImGui::Dummy(size);
    }

    void SetNextItemWidth(f32 width)
    {
        ImGui::SetNextItemWidth(width);
    }

    void PropertyLabel(string_view label)
    {
        const string id = AsCStr(label);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        ImGui::AlignTextToFramePadding();
        ImGui::TextUnformatted(id.c_str(), id.c_str() + id.size());
        ImGui::TableSetColumnIndex(1);
        ImGui::SetNextItemWidth(-FLT_MIN);
    }

    bool PropertyHeader(string_view id, bool defaultOpen)
    {
        const string label = AsCStr(id);
        ImGui::TableNextRow();
        ImGui::TableSetColumnIndex(0);
        // SpanAllColumns stretches the header frame across both property columns; the label
        // also spans, so it is not clipped to the auto-sized label column.
        ImGuiTreeNodeFlags flags =
            ImGuiTreeNodeFlags_SpanAllColumns | ImGuiTreeNodeFlags_LabelSpanAllColumns;
        if (defaultOpen)
        {
            flags |= ImGuiTreeNodeFlags_DefaultOpen;
        }
        return ImGui::CollapsingHeader(label.c_str(), flags);
    }

    f32 GetFrameHeight()
    {
        return ImGui::GetFrameHeight();
    }

    f32 GetTextLineHeight()
    {
        return ImGui::GetTextLineHeight();
    }
}
