#include <Veng/UI/Query.h>

#include <Veng/Assert.h>

#include <imgui.h>

namespace Veng::UI
{
    namespace
    {
        ImGuiMouseButton ToImGui(MouseButton button)
        {
            switch (button)
            {
            case MouseButton::Left:
                return ImGuiMouseButton_Left;
            case MouseButton::Right:
                return ImGuiMouseButton_Right;
            case MouseButton::Middle:
                return ImGuiMouseButton_Middle;
            }
            VE_ASSERT(false, "Unmapped UI::MouseButton");
        }

        ImGuiKey ToImGui(Key key)
        {
            switch (key)
            {
            case Key::Delete:
                return ImGuiKey_Delete;
            case Key::Backspace:
                return ImGuiKey_Backspace;
            case Key::Z:
                return ImGuiKey_Z;
            case Key::S:
                return ImGuiKey_S;
            }
            VE_ASSERT(false, "Unmapped UI::Key");
        }
    }

    bool ItemHovered()
    {
        return ImGui::IsItemHovered();
    }

    bool WindowFocused()
    {
        return ImGui::IsWindowFocused();
    }

    bool WantCaptureMouse()
    {
        return ImGui::GetIO().WantCaptureMouse;
    }

    bool ItemEdited()
    {
        return ImGui::IsItemDeactivatedAfterEdit();
    }

    bool IsItemToggledOpen()
    {
        return ImGui::IsItemToggledOpen();
    }

    bool IsItemClicked(MouseButton button)
    {
        return ImGui::IsItemClicked(ToImGui(button));
    }

    void Tooltip(string_view text)
    {
        if (ImGui::IsItemHovered())
        {
            ImGui::SetTooltip("%.*s", static_cast<int>(text.size()), text.data());
        }
    }

    bool IsMouseClicked(MouseButton button)
    {
        return ImGui::IsMouseClicked(ToImGui(button));
    }

    bool IsMouseDoubleClicked(MouseButton button)
    {
        return ImGui::IsMouseDoubleClicked(ToImGui(button));
    }

    bool IsKeyPressed(Key key)
    {
        return ImGui::IsKeyPressed(ToImGui(key));
    }

    bool IsCtrlDown()
    {
        return ImGui::GetIO().KeyCtrl;
    }

    bool IsShiftDown()
    {
        return ImGui::GetIO().KeyShift;
    }

    bool IsSuperDown()
    {
        return ImGui::GetIO().KeySuper;
    }

    vec2 PopupMousePosition()
    {
        return ImGui::GetMousePosOnOpeningCurrentPopup();
    }

    f32 FrameRate()
    {
        return ImGui::GetIO().Framerate;
    }
}
