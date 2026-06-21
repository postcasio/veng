#include <Veng/UI/Scopes.h>

#include <Veng/Assert.h>

#include <imgui.h>

namespace Veng::UI
{
    namespace
    {
        // ImGui takes const char*, not string_view; materialize at the call boundary.
        string AsCStr(string_view s)
        {
            return string(s);
        }

        ImGuiWindowFlags ToImGui(WindowFlags flags)
        {
            ImGuiWindowFlags out = ImGuiWindowFlags_None;
            if ((flags & WindowFlags::NoScrollbar) != WindowFlags::None)
            {
                out |= ImGuiWindowFlags_NoScrollbar;
            }
            if ((flags & WindowFlags::NoScrollWithMouse) != WindowFlags::None)
            {
                out |= ImGuiWindowFlags_NoScrollWithMouse;
            }
            if ((flags & WindowFlags::HorizontalScrollbar) != WindowFlags::None)
            {
                out |= ImGuiWindowFlags_HorizontalScrollbar;
            }
            return out;
        }

        ImGuiTreeNodeFlags ToImGui(TreeFlags flags)
        {
            ImGuiTreeNodeFlags out = ImGuiTreeNodeFlags_None;
            if ((flags & TreeFlags::DefaultOpen) != TreeFlags::None)
            {
                out |= ImGuiTreeNodeFlags_DefaultOpen;
            }
            if ((flags & TreeFlags::SpanAvailWidth) != TreeFlags::None)
            {
                out |= ImGuiTreeNodeFlags_SpanAvailWidth;
            }
            if ((flags & TreeFlags::Selected) != TreeFlags::None)
            {
                out |= ImGuiTreeNodeFlags_Selected;
            }
            if ((flags & TreeFlags::Leaf) != TreeFlags::None)
            {
                out |= ImGuiTreeNodeFlags_Leaf;
            }
            if ((flags & TreeFlags::OpenOnArrow) != TreeFlags::None)
            {
                out |= ImGuiTreeNodeFlags_OpenOnArrow;
            }
            if ((flags & TreeFlags::AllowOverlap) != TreeFlags::None)
            {
                out |= ImGuiTreeNodeFlags_AllowOverlap;
            }
            return out;
        }

        ImGuiCol ToImGui(StyleColorId id)
        {
            switch (id)
            {
            case StyleColorId::Text:
                return ImGuiCol_Text;
            case StyleColorId::Button:
                return ImGuiCol_Button;
            case StyleColorId::FrameBg:
                return ImGuiCol_FrameBg;
            }
            VE_ASSERT(false, "unmapped StyleColorId {}", static_cast<u32>(id));
        }

        ImGuiStyleVar ToImGui(StyleVarId id)
        {
            switch (id)
            {
            case StyleVarId::WindowPadding:
                return ImGuiStyleVar_WindowPadding;
            case StyleVarId::FramePadding:
                return ImGuiStyleVar_FramePadding;
            case StyleVarId::ItemSpacing:
                return ImGuiStyleVar_ItemSpacing;
            }
            VE_ASSERT(false, "unmapped StyleVarId {}", static_cast<u32>(id));
        }
    }

    ScopedWindow::~ScopedWindow()
    {
        if (m_Live)
        {
            ImGui::End();
        }
    }

    ScopedChild::~ScopedChild()
    {
        if (m_Live)
        {
            ImGui::EndChild();
        }
    }

    ScopedTree::~ScopedTree()
    {
        if (m_Live && m_Open && m_Pop)
        {
            ImGui::TreePop();
        }
    }

    ScopedTable::~ScopedTable()
    {
        if (m_Live && m_Open)
        {
            ImGui::EndTable();
        }
    }

    ScopedMenuBar::~ScopedMenuBar()
    {
        if (m_Live && m_Open)
        {
            ImGui::EndMainMenuBar();
        }
    }

    ScopedMenu::~ScopedMenu()
    {
        if (m_Live && m_Open)
        {
            ImGui::EndMenu();
        }
    }

    ScopedPopup::~ScopedPopup()
    {
        if (m_Live && m_Open)
        {
            ImGui::EndPopup();
        }
    }

    ScopedDragDropSource::~ScopedDragDropSource()
    {
        if (m_Live && m_Open)
        {
            ImGui::EndDragDropSource();
        }
    }

    ScopedDragDropTarget::~ScopedDragDropTarget()
    {
        if (m_Live && m_Open)
        {
            ImGui::EndDragDropTarget();
        }
    }

    DisabledScope::~DisabledScope()
    {
        if (m_Live)
        {
            ImGui::EndDisabled();
        }
    }

    IdScope::~IdScope()
    {
        if (m_Live)
        {
            ImGui::PopID();
        }
    }

    StyleColorScope::~StyleColorScope()
    {
        if (m_Live)
        {
            ImGui::PopStyleColor();
        }
    }

    StyleVarScope::~StyleVarScope()
    {
        if (m_Live)
        {
            ImGui::PopStyleVar();
        }
    }

    ScopedWindow Window(string_view title, bool* open, WindowFlags flags)
    {
        const string id = AsCStr(title);
        return ScopedWindow(ImGui::Begin(id.c_str(), open, ToImGui(flags)));
    }

    ScopedChild Child(string_view id, vec2 size, WindowFlags flags)
    {
        const string label = AsCStr(id);
        return ScopedChild(
            ImGui::BeginChild(label.c_str(), size, ImGuiChildFlags_None, ToImGui(flags)));
    }

    ScopedTree TreeNode(string_view label, TreeFlags flags)
    {
        const string id = AsCStr(label);
        return ScopedTree(ImGui::TreeNodeEx(id.c_str(), ToImGui(flags)), true);
    }

    ScopedTree CollapsingHeader(string_view label, TreeFlags flags)
    {
        const string id = AsCStr(label);
        return ScopedTree(ImGui::CollapsingHeader(id.c_str(), ToImGui(flags)), false);
    }

    ScopedTable Table(string_view id, i32 columns)
    {
        const string label = AsCStr(id);
        return ScopedTable(ImGui::BeginTable(label.c_str(), columns));
    }

    ScopedTable PropertyTable(string_view id)
    {
        const string label = AsCStr(id);
        const bool open = ImGui::BeginTable(label.c_str(), 2);
        if (open)
        {
            ImGui::TableSetupColumn("##label", ImGuiTableColumnFlags_WidthFixed);
            ImGui::TableSetupColumn("##value", ImGuiTableColumnFlags_WidthStretch);
        }
        return ScopedTable(open);
    }

    void TableSetupColumn(string_view label)
    {
        const string id = AsCStr(label);
        ImGui::TableSetupColumn(id.c_str());
    }

    void TableHeadersRow()
    {
        ImGui::TableHeadersRow();
    }

    void TableNextRow()
    {
        ImGui::TableNextRow();
    }

    void TableNextColumn()
    {
        ImGui::TableNextColumn();
    }

    void TableSetColumnIndex(i32 column)
    {
        ImGui::TableSetColumnIndex(column);
    }

    ScopedMenuBar MainMenuBar()
    {
        return ScopedMenuBar(ImGui::BeginMainMenuBar());
    }

    ScopedMenu Menu(string_view label)
    {
        const string id = AsCStr(label);
        return ScopedMenu(ImGui::BeginMenu(id.c_str()));
    }

    bool MenuItem(string_view label, bool enabled)
    {
        const string id = AsCStr(label);
        return ImGui::MenuItem(id.c_str(), nullptr, false, enabled);
    }

    bool MenuItem(string_view label, bool* selected)
    {
        const string id = AsCStr(label);
        return ImGui::MenuItem(id.c_str(), nullptr, selected);
    }

    ScopedPopup Popup(string_view id)
    {
        const string label = AsCStr(id);
        return ScopedPopup(ImGui::BeginPopup(label.c_str()));
    }

    void OpenPopup(string_view id)
    {
        const string label = AsCStr(id);
        ImGui::OpenPopup(label.c_str());
    }

    ScopedPopup PopupContextItem(string_view id)
    {
        const string label = AsCStr(id);
        return ScopedPopup(ImGui::BeginPopupContextItem(label.c_str()));
    }

    ScopedPopup PopupContextWindow(string_view id)
    {
        const string label = AsCStr(id);
        return ScopedPopup(ImGui::BeginPopupContextWindow(label.c_str()));
    }

    ScopedDragDropSource DragDropSource()
    {
        return ScopedDragDropSource(ImGui::BeginDragDropSource());
    }

    void SetDragDropPayload(string_view type, const void* data, usize size)
    {
        const string id = AsCStr(type);
        ImGui::SetDragDropPayload(id.c_str(), data, size);
    }

    ScopedDragDropTarget DragDropTarget()
    {
        return ScopedDragDropTarget(ImGui::BeginDragDropTarget());
    }

    const void* AcceptDragDropPayload(string_view type)
    {
        const string id = AsCStr(type);
        const ImGuiPayload* payload = ImGui::AcceptDragDropPayload(id.c_str());
        return payload != nullptr ? payload->Data : nullptr;
    }

    DisabledScope Disabled(bool disabled)
    {
        ImGui::BeginDisabled(disabled);
        return DisabledScope{};
    }

    IdScope PushId(string_view id)
    {
        const string label = AsCStr(id);
        ImGui::PushID(label.c_str());
        return IdScope{};
    }

    StyleColorScope StyleColor(StyleColorId id, vec4 color)
    {
        ImGui::PushStyleColor(ToImGui(id), color);
        return StyleColorScope{};
    }

    StyleVarScope StyleVar(StyleVarId id, vec2 value)
    {
        ImGui::PushStyleVar(ToImGui(id), value);
        return StyleVarScope{};
    }

    StyleVarScope StyleVar(StyleVarId id, f32 value)
    {
        ImGui::PushStyleVar(ToImGui(id), value);
        return StyleVarScope{};
    }
}
