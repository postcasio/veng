#pragma once
#include <Veng/Veng.h>

// Veng::UI item/hover queries, immediate-mode input queries, and a thin stats
// readout. The input queries front ImGui's per-frame input state for UI logic
// (double-click to open, right-click menu, delete-key shortcut) — distinct from
// the event/input system, which feeds gameplay. imgui-free signatures; impl in
// Query.cpp.

namespace Veng::UI
{
    // Mouse buttons a query names. Maps to ImGuiMouseButton_ in the impl.
    enum class MouseButton : u32
    {
        Left,
        Right,
        Middle,
    };

    // Keys a query names — a closed engine enum mapped to ImGuiKey_ in the impl.
    // Populated to the keys the call sites use.
    enum class Key : u32
    {
        Delete,
        Backspace,
    };

    // True if the last item is hovered.
    [[nodiscard]] bool ItemHovered();

    // True the frame the last item was deactivated after an edit.
    [[nodiscard]] bool ItemEdited();

    // Shows a tooltip with text when the last item is hovered.
    void Tooltip(string_view text);

    // True the frame button was pressed (clicked down) this frame.
    [[nodiscard]] bool IsMouseClicked(MouseButton button);

    // True the frame button completed a double-click this frame.
    [[nodiscard]] bool IsMouseDoubleClicked(MouseButton button);

    // True the frame key transitioned to pressed (with key-repeat).
    [[nodiscard]] bool IsKeyPressed(Key key);

    // The mouse position captured when the current popup was opened — stable while
    // the popup stays open, so newly placed content anchors to the open point.
    [[nodiscard]] vec2 PopupMousePosition();

    // The running average frame rate (frames per second) of the UI backend.
    [[nodiscard]] f32 FrameRate();
}
