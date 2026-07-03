#pragma once
#include <Veng/Veng.h>

#include <imgui.h>

/// @brief Internal shared state for the `UI::Joined` widget-fusing scope.
///
/// A `Joined` scope makes adjacent `Veng::UI` widgets read as one fused control: each
/// instrumented widget wrapper calls `JoinedPreItem` before drawing and `JoinedPostItem`
/// after, so the machinery places every item after the first hard against the previous one
/// (`SameLine(0, 0)`). Every widget in a group draws square-cornered: stock framed widgets via
/// a pushed zero `FrameRounding` (the artifact-free treatment for the bordered theme, where
/// patching a rounded border corner would glitch), custom-drawn widgets via `JoinedCornerFlags`
/// returning no rounding while a group is active — an immediate-mode widget cannot know it is
/// the group's last item, so any rounded edge could land mid-group. This header names imgui
/// types and so lives under `engine/src/UI/`, never in a public header.

namespace Veng::UI
{
    /// @brief Opens a joined group: pushes the id and zero spacing/frame-rounding, resets the counter.
    ///
    /// @param id  ImGui id string scoping the group's widgets.
    /// @pre No joined group is already active — nesting is asserted against by the caller.
    void JoinedBegin(string_view id);

    /// @brief Closes the joined group and restores the pushed spacing/rounding state.
    void JoinedEnd();

    /// @brief Whether a joined group is currently active.
    [[nodiscard]] bool JoinedActive();

    /// @brief Runs the per-item pre-hook for an instrumented widget inside a joined group.
    ///
    /// A no-op outside a group. Inside one, every item after the first is placed hard against
    /// the previous item with `SameLine(0, 0)`.
    void JoinedPreItem();

    /// @brief Runs the per-item post-hook: advances the group's item counter.
    ///
    /// A no-op outside a group.
    void JoinedPostItem();

    /// @brief Resolves the corner-rounding flags for a custom-drawn widget.
    ///
    /// Outside a group the widget's own intent is honored (a standalone `IconButton` rounds both
    /// sides, a `ButtonGroup` rounds only its outer segments' outer edges). Inside an active
    /// group every widget squares, matching the flattened stock frames.
    /// @param roundLeft   Whether the widget's left corners round when standalone.
    /// @param roundRight  Whether the widget's right corners round when standalone.
    /// @return The `ImDrawFlags` corner-rounding mask.
    [[nodiscard]] ImDrawFlags JoinedCornerFlags(bool roundLeft, bool roundRight);
}
