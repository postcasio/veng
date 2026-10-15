#pragma once
#include <Veng/Veng.h>

/// @brief Cursor and spacing layout helpers for widget authors.
///
/// ImGui-free signatures; impl in Layout.cpp.

namespace Veng::UI
{
    /// @brief Draws a horizontal separator line.
    VE_API void Separator();

    /// @brief Advances the cursor to the same line as the previous widget.
    VE_API void SameLine();

    /// @brief Vertically centers the next text item against a framed widget on the same line.
    ///
    /// Offsets the cursor down by the frame padding so a following `Text` aligns with an
    /// inline framed widget (a button, input, or progress bar) rather than sitting at its top
    /// edge. Call immediately before the text item.
    VE_API void AlignTextToFramePadding();

    /// @brief Inserts a blank line of spacing.
    VE_API void Spacing();

    /// @brief Returns the space remaining in the current content region.
    [[nodiscard]] VE_API vec2 ContentRegionAvail();

    /// @brief Returns the cursor position, in window-local content coordinates.
    ///
    /// Pairs with `SetCursorPos` to lay a later widget back over an earlier one — an
    /// icon-grid cell overlays its badge and label inside a sized selectable's rect.
    [[nodiscard]] VE_API vec2 CursorPos();

    /// @brief Sets the cursor position, in window-local content coordinates.
    /// @param pos  Position in the same space `CursorPos` returns.
    VE_API void SetCursorPos(vec2 pos);

    /// @brief Scrolls so the current cursor position is visible.
    VE_API void ScrollToHere();

    /// @brief Increases the cursor indentation for following widgets.
    VE_API void Indent();

    /// @brief Decreases the cursor indentation back toward the margin.
    VE_API void Unindent();

    /// @brief Reserves an empty rectangle of layout space.
    ///
    /// Advances the cursor without drawing, leaving a gap (a drop-target row between
    /// tree entries, for instance).
    /// @param size  Width and height of the reserved space in pixels.
    VE_API void Dummy(vec2 size);

    /// @brief Sets the width of the next widget.
    ///
    /// A negative width fills the remaining content region (e.g. a full-width search box);
    /// a positive width is taken in pixels.
    /// @param width  Next-item width in pixels, or negative to fill the remaining space.
    VE_API void SetNextItemWidth(f32 width);

    /// @brief Begins a property-table row: a label in column 0, the next widget in column 1.
    ///
    /// Call inside a `PropertyTable` scope. Advances to a new row, draws `label`
    /// left-aligned and frame-aligned in the label column, then moves to the value column
    /// and sets the next item to fill it — so the caller's following `UI::Drag(...)` /
    /// `UI::InputText(...)` lands stretched in column 1:
    /// `if (auto t = UI::PropertyTable("xform")) { UI::PropertyLabel("Position"); UI::Drag("##pos", pos); }`
    /// @param label  Property name drawn in the label column.
    VE_API void PropertyLabel(string_view label);

    /// @brief Draws a full-width collapsing-header row spanning every column of a property table.
    ///
    /// Advances to a new row and draws a `CollapsingHeader` whose frame spans the whole table
    /// width (label starting in column 0), so a section header sits cleanly between the
    /// two-column `label : widget` rows without opening a nested table inside a cell. Call
    /// inside a `PropertyTable` scope; draw the section's rows only when it returns true. The
    /// header's open/closed state is ImGui-owned, keyed by `id` — `defaultOpen` only seeds the
    /// first frame.
    /// @param id           Header label and ImGui id (prefix with `##` to hide the visible label).
    /// @param defaultOpen  Whether the section starts expanded on its first frame.
    /// @return True when the section is open and its rows should be drawn.
    [[nodiscard]] VE_API bool PropertyHeader(string_view id, bool defaultOpen = true);

    /// @brief Returns the height of a framed widget (one text line plus frame padding), in pixels.
    ///
    /// The vertical extent of a button, header, or input — used to position content overlaid
    /// on such a widget.
    [[nodiscard]] VE_API f32 GetFrameHeight();

    /// @brief Returns the height of one line of text, in pixels.
    [[nodiscard]] VE_API f32 GetTextLineHeight();

    /// @brief Returns the pixel size the given text would occupy if drawn.
    ///
    /// The extent `Text(text)` would take at the current font, measuring only — nothing is drawn.
    /// A caller right-aligns a label by subtracting the width from the content region.
    /// @param text  The string to measure.
    /// @return The width and height of the text in pixels.
    [[nodiscard]] VE_API vec2 CalcTextSize(string_view text);
}
