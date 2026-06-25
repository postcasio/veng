#pragma once
#include <Veng/Veng.h>

/// @brief The `Veng::UI` theme: a semantic design palette the UI layer reads its colors
///        and metrics from.
///
/// The theme is a small set of *semantic roles* (surfaces, text, accent, status) plus
/// layout metrics — a design system, not a one-to-one dump of ImGui's ~60 color slots.
/// The ImGui integration (`ImGuiLayer`) derives the full ImGui/imnodes style from these
/// roles, and widget code that needs a color (a log level, a status tint) reads the role
/// straight off `GetTheme()`. No imgui types appear here: the surface names only engine
/// types, keeping the header within `include_hygiene`'s guarantee.

namespace Veng::UI
{
    /// @brief A semantic UI palette plus layout metrics.
    ///
    /// Colors are linear-ish sRGB `vec4`s (RGBA, components in `[0, 1]`). The surface roles
    /// run deepest-to-raised (`Background` → `SurfaceActive`); the accent and status roles
    /// are the chromatic highlights. Metrics are in ImGui's logical pixels.
    struct Theme
    {
        /// @brief The deepest backdrop: the main window and dockspace background.
        vec4 Background;
        /// @brief Base surface for panels, popups, and input frames.
        vec4 Surface;
        /// @brief A raised surface: buttons, headers, tabs, scrollbar grabs at rest.
        vec4 SurfaceRaised;
        /// @brief A raised surface under the cursor (hover state).
        vec4 SurfaceHovered;
        /// @brief A raised surface while pressed or held (active state).
        vec4 SurfaceActive;
        /// @brief Default separator and frame border color.
        vec4 Border;
        /// @brief A heavier border for emphasis (strong table rules, outlines).
        vec4 BorderStrong;

        /// @brief Primary foreground text.
        vec4 Text;
        /// @brief Secondary, de-emphasized text (captions, hints, inactive labels).
        vec4 TextMuted;
        /// @brief Disabled-control text.
        vec4 TextDisabled;
        /// @brief Text drawn over an accent-filled element (selected tab, active button).
        vec4 TextOnAccent;

        /// @brief The primary brand accent: highlights, checks, slider grabs, selection.
        vec4 Accent;
        /// @brief The accent under the cursor (hover state).
        vec4 AccentHovered;
        /// @brief The accent while pressed or held (active state).
        vec4 AccentActive;
        /// @brief A low-alpha accent for subtle fills: selection bands, drag-drop targets.
        vec4 AccentMuted;

        /// @brief Positive/confirmation status color.
        vec4 Success;
        /// @brief Caution status color.
        vec4 Warning;
        /// @brief Failure/destructive status color.
        vec4 Error;
        /// @brief Neutral-informational status color.
        vec4 Info;

        /// @brief Corner radius of top-level windows.
        f32 WindowRounding;
        /// @brief Corner radius of child regions.
        f32 ChildRounding;
        /// @brief Corner radius of input frames, buttons, and headers.
        f32 FrameRounding;
        /// @brief Corner radius of popups and tooltips.
        f32 PopupRounding;
        /// @brief Corner radius of slider/scrollbar grabs.
        f32 GrabRounding;
        /// @brief Corner radius of tabs.
        f32 TabRounding;
        /// @brief Corner radius of scrollbars.
        f32 ScrollbarRounding;
        /// @brief Thickness of frame and window borders, in pixels (`0` disables borders).
        f32 BorderSize;

        /// @brief Padding inside window borders.
        vec2 WindowPadding;
        /// @brief Padding inside widget frames.
        vec2 FramePadding;
        /// @brief Spacing between consecutive widgets.
        vec2 ItemSpacing;
        /// @brief Spacing between a widget and its inline label/sub-parts.
        vec2 ItemInnerSpacing;
        /// @brief Width of vertical scrollbars / height of horizontal ones.
        f32 ScrollbarSize;
        /// @brief Minimum length of a slider/scrollbar grab.
        f32 GrabMinSize;

        /// @brief Whether a window's title bar shows the collapse-toggle arrow.
        ///
        /// When `false`, the title bar carries no collapse button; a window still collapses
        /// by double-clicking its title bar. When `true`, the arrow is shown at the title
        /// bar's leading edge.
        bool ShowWindowCollapseButton;
    };

    /// @brief Returns the built-in dark theme.
    ///
    /// A clean, cool-neutral dark palette with a single azure accent. This is the default
    /// the UI starts on and the fallback a future theme loader resets to.
    /// @return The built-in dark theme, a reference to a function-local static.
    [[nodiscard]] const Theme& BuiltInDarkTheme();

    /// @brief Returns the theme the UI layer currently reads from.
    ///
    /// Widget code that needs a semantic color reads it off this. Defaults to
    /// `BuiltInDarkTheme()` until `SetTheme` is called.
    /// @return The active theme.
    [[nodiscard]] const Theme& GetTheme();

    /// @brief Replaces the active theme.
    ///
    /// Updates the value `GetTheme()` returns. The ImGui/imnodes style is refreshed
    /// separately by `ImGuiLayer::ApplyTheme()` — a host that swaps the theme mid-run calls
    /// that afterward to push the change into the live style.
    /// @param theme  The new active theme.
    void SetTheme(const Theme& theme);

    /// @brief Converts an sRGB-encoded color to linear, leaving alpha untouched.
    ///
    /// Theme and widget colors are authored in sRGB (the universal hex convention), but the
    /// engine's UI flows through a linear pipeline: ImGui renders into a linear float target
    /// the display re-encodes to sRGB at scanout. So every authored color is linearized at
    /// the ImGui boundary (the style/imnodes setup and the color-taking widgets) to round-trip
    /// to its intended brightness. UI code computing a raw color to hand to ImGui uses this.
    /// @param color  An sRGB color (RGB encoded, A linear coverage).
    /// @return The color with its RGB linearized and its alpha preserved.
    [[nodiscard]] vec4 SrgbToLinear(vec4 color);
}
