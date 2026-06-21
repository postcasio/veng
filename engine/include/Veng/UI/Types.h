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
}
