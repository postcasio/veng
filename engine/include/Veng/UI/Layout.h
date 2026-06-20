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

    /// @brief Scrolls so the current cursor position is visible.
    void ScrollToHere();
}
