#include <Veng/UI/Scopes.h>
#include <Veng/UI/Theme.h>

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

    ScopedCombo::~ScopedCombo()
    {
        if (m_Live && m_Open)
        {
            ImGui::EndCombo();
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

    ScopedWindow ViewportOverlay(string_view id, OverlayAnchor anchor, vec2 padding)
    {
        const string label = AsCStr(id);
        const Theme& theme = GetTheme();

        // The overlay pins to the parent window's content rect — the region the viewport
        // image occupies — not the whole window, so the panel clears the title bar/borders.
        const vec2 windowPos = ImGui::GetWindowPos();
        const vec2 regionMin = ImGui::GetWindowContentRegionMin();
        const vec2 regionMax = ImGui::GetWindowContentRegionMax();
        const vec2 contentMin = windowPos + regionMin;
        const vec2 contentMax = windowPos + regionMax;

        // Pivot and anchor point pair up: pivot picks which corner of the panel lands on
        // the anchor point, so the panel always stays inside the content rect.
        vec2 anchorPoint{0.0f, 0.0f};
        vec2 pivot{0.0f, 0.0f};
        const f32 centerX = (contentMin.x + contentMax.x) * 0.5f;
        switch (anchor)
        {
        case OverlayAnchor::TopLeft:
            anchorPoint = vec2(contentMin.x + padding.x, contentMin.y + padding.y);
            pivot = vec2(0.0f, 0.0f);
            break;
        case OverlayAnchor::TopCenter:
            anchorPoint = vec2(centerX, contentMin.y + padding.y);
            pivot = vec2(0.5f, 0.0f);
            break;
        case OverlayAnchor::TopRight:
            anchorPoint = vec2(contentMax.x - padding.x, contentMin.y + padding.y);
            pivot = vec2(1.0f, 0.0f);
            break;
        case OverlayAnchor::BottomLeft:
            anchorPoint = vec2(contentMin.x + padding.x, contentMax.y - padding.y);
            pivot = vec2(0.0f, 1.0f);
            break;
        case OverlayAnchor::BottomCenter:
            anchorPoint = vec2(centerX, contentMax.y - padding.y);
            pivot = vec2(0.5f, 1.0f);
            break;
        case OverlayAnchor::BottomRight:
            anchorPoint = vec2(contentMax.x - padding.x, contentMax.y - padding.y);
            pivot = vec2(1.0f, 1.0f);
            break;
        }

        ImGui::SetNextWindowPos(anchorPoint, ImGuiCond_Always, pivot);

        vec4 fill = theme.SurfaceRaised;
        fill.a *= 0.85f;
        ImGui::PushStyleColor(ImGuiCol_WindowBg, SrgbToLinear(fill));
        ImGui::PushStyleVar(ImGuiStyleVar_WindowRounding, theme.WindowRounding);

        const ImGuiWindowFlags flags =
            ImGuiWindowFlags_NoDecoration | ImGuiWindowFlags_NoMove |
            ImGuiWindowFlags_NoSavedSettings | ImGuiWindowFlags_AlwaysAutoResize |
            ImGuiWindowFlags_NoDocking | ImGuiWindowFlags_NoFocusOnAppearing;
        const bool open = ImGui::Begin(label.c_str(), nullptr, flags);

        ImGui::PopStyleVar();
        ImGui::PopStyleColor();
        return ScopedWindow(open);
    }

    ScopedTree TreeNode(string_view label, TreeFlags flags)
    {
        const string id = AsCStr(label);
        return ScopedTree(ImGui::TreeNodeEx(id.c_str(), ToImGui(flags)), true);
    }

    ScopedTree CollapsingHeader(string_view label, TreeFlags flags)
    {
        const string id = AsCStr(label);
        const Theme& theme = GetTheme();

        // A collapsing header reads as a neutral surface button, not an accent-filled band, and
        // tightens its horizontal frame padding so the arrow and label sit close to the edge —
        // both scoped to this widget rather than the global Header slots (shared by selectables,
        // tree-node selection, and menu items).
        ImGui::PushStyleColor(ImGuiCol_Header, SrgbToLinear(theme.SurfaceRaised));
        ImGui::PushStyleColor(ImGuiCol_HeaderHovered, SrgbToLinear(theme.SurfaceHovered));
        ImGui::PushStyleColor(ImGuiCol_HeaderActive, SrgbToLinear(theme.SurfaceActive));
        ImGui::PushStyleVar(ImGuiStyleVar_FramePadding,
                            ImVec2(theme.FramePadding.x * 0.5f, theme.FramePadding.y));

        const bool open = ImGui::CollapsingHeader(id.c_str(), ToImGui(flags));

        ImGui::PopStyleVar();
        ImGui::PopStyleColor(3);
        return ScopedTree(open, false);
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

    ScopedCombo ComboBox(string_view id, string_view preview)
    {
        const string label = AsCStr(id);
        const string previewText = AsCStr(preview);
        return ScopedCombo(ImGui::BeginCombo(label.c_str(), previewText.c_str()));
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
        // The caller's color is authored sRGB; linearize for the linear UI pipeline.
        ImGui::PushStyleColor(ToImGui(id), SrgbToLinear(color));
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
