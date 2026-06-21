#pragma once
#include <Veng/Veng.h>

/// @brief Item/hover queries, immediate-mode input queries, and a frame-rate readout.
///
/// The input queries front ImGui's per-frame input state for UI logic (double-click to
/// open, right-click menu, delete-key shortcut) — distinct from the event/input system,
/// which feeds gameplay. ImGui-free signatures; impl in Query.cpp.

namespace Veng::UI
{
    /// @brief Mouse buttons a query names.
    ///
    /// Maps to ImGuiMouseButton_ in the impl.
    enum class MouseButton : u32
    {
        /// @brief Left mouse button.
        Left,
        /// @brief Right mouse button.
        Right,
        /// @brief Middle mouse button (scroll wheel click).
        Middle,
    };

    /// @brief Keys a query names — a closed engine enum mapped to ImGuiKey_ in the impl.
    enum class Key : u32
    {
        /// @brief Delete key.
        Delete,
        /// @brief Backspace key.
        Backspace,
    };

    /// @brief Returns true if the last item is hovered.
    [[nodiscard]] bool ItemHovered();

    /// @brief Returns true if the current window has interaction focus.
    ///
    /// For a panel gating its own input (e.g. a viewport driving an editor camera only
    /// while focused) — distinct from item hover.
    [[nodiscard]] bool WindowFocused();

    /// @brief Returns true the frame the last item was clicked with the given button.
    /// @param button  Mouse button to query.
    [[nodiscard]] bool IsItemClicked(MouseButton button = MouseButton::Left);

    /// @brief Returns true the frame the last item was deactivated after an edit.
    [[nodiscard]] bool ItemEdited();

    /// @brief Returns true the frame the last tree node's open state toggled.
    ///
    /// Lets a hierarchy panel tell an arrow-expand from a selection click on the same row.
    [[nodiscard]] bool IsItemToggledOpen();

    /// @brief Shows a tooltip with text when the last item is hovered.
    /// @param text  Tooltip content.
    void Tooltip(string_view text);

    /// @brief Returns true the frame the button was clicked down.
    /// @param button  Mouse button to query.
    [[nodiscard]] bool IsMouseClicked(MouseButton button);

    /// @brief Returns true the frame the button completed a double-click.
    /// @param button  Mouse button to query.
    [[nodiscard]] bool IsMouseDoubleClicked(MouseButton button);

    /// @brief Returns true the frame the key transitioned to pressed (with key-repeat).
    /// @param key  Key to query.
    [[nodiscard]] bool IsKeyPressed(Key key);

    /// @brief Returns true while either Ctrl key is held.
    ///
    /// For multi-select toggle modifiers in list/tree panels.
    [[nodiscard]] bool IsCtrlDown();

    /// @brief Returns true while either Shift key is held.
    ///
    /// For range-select modifiers in list/tree panels.
    [[nodiscard]] bool IsShiftDown();

    /// @brief Returns the mouse position captured when the current popup was opened.
    ///
    /// Stable while the popup stays open, so newly placed content anchors to the open point.
    [[nodiscard]] vec2 PopupMousePosition();

    /// @brief Returns the running average frame rate (frames per second) of the UI backend.
    [[nodiscard]] f32 FrameRate();
}
