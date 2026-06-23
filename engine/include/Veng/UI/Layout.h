#pragma once
#include <Veng/Veng.h>

/// @brief Cursor and spacing layout helpers for widget authors.
///
/// ImGui-free signatures; impl in Layout.cpp.

namespace Veng::UI
{
    /// @brief Draws a horizontal separator line.
    void Separator();

    /// @brief Advances the cursor to the same line as the previous widget.
    void SameLine();

    /// @brief Inserts a blank line of spacing.
    void Spacing();

    /// @brief Returns the space remaining in the current content region.
    [[nodiscard]] vec2 ContentRegionAvail();

    /// @brief Returns the cursor position, in window-local content coordinates.
    ///
    /// Pairs with `SetCursorPos` to lay a later widget back over an earlier one — an
    /// icon-grid cell overlays its badge and label inside a sized selectable's rect.
    [[nodiscard]] vec2 CursorPos();

    /// @brief Sets the cursor position, in window-local content coordinates.
    /// @param pos  Position in the same space `CursorPos` returns.
    void SetCursorPos(vec2 pos);

    /// @brief Scrolls so the current cursor position is visible.
    void ScrollToHere();

    /// @brief Increases the cursor indentation for following widgets.
    void Indent();

    /// @brief Decreases the cursor indentation back toward the margin.
    void Unindent();

    /// @brief Reserves an empty rectangle of layout space.
    ///
    /// Advances the cursor without drawing, leaving a gap (a drop-target row between
    /// tree entries, for instance).
    /// @param size  Width and height of the reserved space in pixels.
    void Dummy(vec2 size);

    /// @brief Sets the width of the next widget.
    ///
    /// A negative width fills the remaining content region (e.g. a full-width search box);
    /// a positive width is taken in pixels.
    /// @param width  Next-item width in pixels, or negative to fill the remaining space.
    void SetNextItemWidth(f32 width);

    /// @brief Begins a property-table row: a label in column 0, the next widget in column 1.
    ///
    /// Call inside a `PropertyTable` scope. Advances to a new row, draws `label`
    /// left-aligned and frame-aligned in the label column, then moves to the value column
    /// and sets the next item to fill it — so the caller's following `UI::Drag(...)` /
    /// `UI::InputText(...)` lands stretched in column 1:
    /// `if (auto t = UI::PropertyTable("xform")) { UI::PropertyLabel("Position"); UI::Drag("##pos", pos); }`
    /// @param label  Property name drawn in the label column.
    void PropertyLabel(string_view label);

    /// @brief Returns the height of a framed widget (one text line plus frame padding), in pixels.
    ///
    /// The vertical extent of a button, header, or input — used to position content overlaid
    /// on such a widget.
    [[nodiscard]] f32 GetFrameHeight();

    /// @brief Returns the height of one line of text, in pixels.
    [[nodiscard]] f32 GetTextLineHeight();
}
