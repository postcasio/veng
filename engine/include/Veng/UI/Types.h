#pragma once
#include <Veng/Veng.h>

/// @brief Vocabulary types and options structs for the `Veng::UI` widget free functions.
///
/// No imgui types appear here: the surface names only engine types, and the matching
/// ImGui flags/format spellings are produced in `engine/src/UI/*.cpp`.

namespace Veng::UI
{
    /// @brief Configuration for drag-edit widgets.
    ///
    /// `Min`/`Max` are optional: `nullopt` means unclamped. A present min/max maps to
    /// ImGui's clamp bounds and the `AlwaysClamp` flag in the implementation.
    ///
    /// `Format` is a printf numeric spec (ImGui's own value-formatting, not user text).
    /// `nullptr` lets each `Drag` overload pick its type-appropriate default (`"%.3f"` for
    /// float overloads, `"%d"` for the integer one), so one `DragOptions` serves both; a
    /// caller may override with an explicit spec.
    struct DragOptions
    {
        /// @brief Drag speed in units per pixel.
        f32 Speed = 0.01f;
        /// @brief Lower clamp bound; `nullopt` for unclamped.
        optional<f32> Min;
        /// @brief Upper clamp bound; `nullopt` for unclamped.
        optional<f32> Max;
        /// @brief Printf format string; `nullptr` uses the per-type default.
        const char* Format = nullptr;
    };

    /// @brief Configuration for slider-edit widgets.
    ///
    /// A slider without a declared range is meaningless, so the float `Slider` requires
    /// this struct (no default).
    struct SliderOptions
    {
        /// @brief Lower bound of the slider range.
        f32 Min = 0.0f;
        /// @brief Upper bound of the slider range.
        f32 Max = 1.0f;
        /// @brief Printf format string for the displayed value.
        const char* Format = "%.3f";
    };

    /// @brief Configuration for the `PlotLines` graph widget.
    ///
    /// `ScaleMin`/`ScaleMax` are optional: `nullopt` autoscales that bound to the series'
    /// own min/max, while a present value pins the axis so a line's height maps to an
    /// absolute quantity (frame milliseconds, say). `OverlayText` draws centered over the
    /// plot — a current-value readout — and an empty view draws none. `Offset` is the index
    /// of the oldest sample, so a ring buffer plots oldest-to-newest without being rotated.
    struct PlotOptions
    {
        /// @brief Text drawn centered over the plot; an empty view draws none.
        string_view OverlayText;
        /// @brief Lower bound of the value axis; `nullopt` autoscales to the series minimum.
        optional<f32> ScaleMin;
        /// @brief Upper bound of the value axis; `nullopt` autoscales to the series maximum.
        optional<f32> ScaleMax;
        /// @brief Index of the oldest sample for ring-buffer data; 0 plots in array order.
        i32 Offset = 0;
        /// @brief Graph size in pixels; a zero component fills width / takes the default height.
        vec2 Size = {0.0f, 0.0f};
    };

    /// @brief Flags controlling window and child-region display behavior.
    ///
    /// A closed engine enum; the `.cpp` maps each bit to its `ImGuiWindowFlags_` counterpart.
    /// Bitwise-combinable. `UI::Child` accepts the same set (`BeginChild` takes window flags),
    /// so `HorizontalScrollbar` — a child knob — lives here too.
    enum class WindowFlags : u32
    {
        /// @brief No flags.
        None = 0,
        /// @brief Hides the vertical scrollbar.
        NoScrollbar = 1,
        /// @brief Prevents scrolling via the mouse wheel.
        NoScrollWithMouse = 2,
        /// @brief Shows a horizontal scrollbar when content overflows.
        HorizontalScrollbar = 4,
    };

    /// @brief Bitwise OR of two `WindowFlags` values.
    constexpr WindowFlags operator|(WindowFlags a, WindowFlags b)
    {
        return static_cast<WindowFlags>(static_cast<u32>(a) | static_cast<u32>(b));
    }

    /// @brief Bitwise AND of two `WindowFlags` values.
    constexpr WindowFlags operator&(WindowFlags a, WindowFlags b)
    {
        return static_cast<WindowFlags>(static_cast<u32>(a) & static_cast<u32>(b));
    }

    /// @brief Flags controlling tree-node and collapsing-header display behavior.
    enum class TreeFlags : u32
    {
        /// @brief No flags.
        None = 0,
        /// @brief Node starts expanded.
        DefaultOpen = 1,
        /// @brief Hit area spans the full available width.
        SpanAvailWidth = 2,
        /// @brief Draws the node highlighted as the active selection.
        Selected = 4,
        /// @brief Childless node: no arrow, no expansion (a hierarchy leaf).
        Leaf = 8,
        /// @brief Only the arrow toggles expansion; a label click is a plain selection.
        OpenOnArrow = 16,
        /// @brief Lets a later widget overlap the node's span (a button drawn over the row).
        AllowOverlap = 32,
    };

    /// @brief Bitwise OR of two `TreeFlags` values.
    constexpr TreeFlags operator|(TreeFlags a, TreeFlags b)
    {
        return static_cast<TreeFlags>(static_cast<u32>(a) | static_cast<u32>(b));
    }

    /// @brief Bitwise AND of two `TreeFlags` values.
    constexpr TreeFlags operator&(TreeFlags a, TreeFlags b)
    {
        return static_cast<TreeFlags>(static_cast<u32>(a) & static_cast<u32>(b));
    }

    /// @brief Flags controlling selectable-row behavior.
    ///
    /// A closed engine enum; the `.cpp` maps each bit to its `ImGuiSelectableFlags_`
    /// counterpart. Bitwise-combinable.
    enum class SelectableFlags : u32
    {
        /// @brief No flags.
        None = 0,
        /// @brief Hit area spans every column of the enclosing table.
        SpanAllColumns = 1,
        /// @brief Reports a click on the double-click press, so a caller can pair it with
        /// `IsMouseDoubleClicked` to act on a double-click (without it the row reports on
        /// release, which never coincides with the double-click query).
        AllowDoubleClick = 2,
    };

    /// @brief Bitwise OR of two `SelectableFlags` values.
    constexpr SelectableFlags operator|(SelectableFlags a, SelectableFlags b)
    {
        return static_cast<SelectableFlags>(static_cast<u32>(a) | static_cast<u32>(b));
    }

    /// @brief Bitwise AND of two `SelectableFlags` values.
    constexpr SelectableFlags operator&(SelectableFlags a, SelectableFlags b)
    {
        return static_cast<SelectableFlags>(static_cast<u32>(a) & static_cast<u32>(b));
    }

    /// @brief Style color slots a `StyleColor` scope guard pushes.
    ///
    /// A closed engine enum; the `.cpp` maps each value to its `ImGuiCol_` counterpart.
    /// Populated to the slots the call sites use.
    enum class StyleColorId : u32
    {
        /// @brief Main text color.
        Text,
        /// @brief Button background color.
        Button,
        /// @brief Input and widget frame background color.
        FrameBg,
    };

    /// @brief Style variable slots a `StyleVar` scope guard pushes.
    ///
    /// A closed engine enum; the `.cpp` maps each value to its `ImGuiStyleVar_` counterpart.
    /// Populated to the slots the call sites use.
    enum class StyleVarId : u32
    {
        /// @brief Padding inside window borders.
        WindowPadding,
        /// @brief Padding inside widget frames.
        FramePadding,
        /// @brief Horizontal and vertical spacing between widgets.
        ItemSpacing,
    };

    /// @brief Placement of a viewport overlay within its parent window's content region.
    ///
    /// Names one of the six edge/corner anchor points an overlay panel pins to. The
    /// overlay sits inside the current window's content rect, offset from the chosen
    /// edge or corner by its padding.
    enum class OverlayAnchor : u32
    {
        /// @brief Pinned to the top-left corner.
        TopLeft,
        /// @brief Centered along the top edge.
        TopCenter,
        /// @brief Pinned to the top-right corner.
        TopRight,
        /// @brief Pinned to the bottom-left corner.
        BottomLeft,
        /// @brief Centered along the bottom edge.
        BottomCenter,
        /// @brief Pinned to the bottom-right corner.
        BottomRight,
    };
}
