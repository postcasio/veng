#pragma once
#include <Veng/Veng.h>

// Veng::UI vocabulary types — designated-initializer options structs (the XInfo
// idiom) the widget free functions take. No imgui types appear here: the surface
// names only engine types, and the matching ImGui flags/format spellings are
// produced in engine/src/UI/*.cpp.

namespace Veng::UI
{
    // Drag-edit configuration. Min/Max are optional: nullopt means unclamped (the
    // edit ranges freely). A present min/max maps to ImGui's clamp bounds and the
    // AlwaysClamp flag in the implementation.
    //
    // Format is a printf numeric spec (ImGui's own value-formatting, not user
    // text). The default "%.3f" is a float spec; the integer Drag overload
    // substitutes "%d" when Format is this default sentinel, so one DragOptions
    // serves both the float and integer overloads — a caller may still override it.
    struct DragOptions
    {
        f32 Speed = 0.01f;
        optional<f32> Min;
        optional<f32> Max;
        const char* Format = "%.3f";
    };

    // Slider-edit configuration. A slider without a declared range is meaningless,
    // so the float Slider requires this struct (no default).
    struct SliderOptions
    {
        f32 Min = 0.0f;
        f32 Max = 1.0f;
        const char* Format = "%.3f";
    };

    // Window/child knobs. A closed engine enum, not ImGuiWindowFlags — the .cpp
    // maps each bit to its ImGuiWindowFlags_ counterpart. Bitwise-combinable.
    // UI::Child accepts the same set (BeginChild takes window flags), so
    // HorizontalScrollbar — a child knob — lives here too.
    enum class WindowFlags : u32
    {
        None = 0,
        NoScrollbar = 1,
        NoScrollWithMouse = 2,
        HorizontalScrollbar = 4,
    };

    constexpr WindowFlags operator|(WindowFlags a, WindowFlags b)
    {
        return static_cast<WindowFlags>(static_cast<u32>(a) | static_cast<u32>(b));
    }

    constexpr WindowFlags operator&(WindowFlags a, WindowFlags b)
    {
        return static_cast<WindowFlags>(static_cast<u32>(a) & static_cast<u32>(b));
    }

    // Tree-node / collapsing-header knobs.
    enum class TreeFlags : u32
    {
        None = 0,
        DefaultOpen = 1,
        SpanAvailWidth = 2,
    };

    constexpr TreeFlags operator|(TreeFlags a, TreeFlags b)
    {
        return static_cast<TreeFlags>(static_cast<u32>(a) | static_cast<u32>(b));
    }

    constexpr TreeFlags operator&(TreeFlags a, TreeFlags b)
    {
        return static_cast<TreeFlags>(static_cast<u32>(a) & static_cast<u32>(b));
    }

    // Style ids a StyleColor/StyleVar guard pushes — closed engine enums the .cpp
    // maps to ImGuiCol_ / ImGuiStyleVar_. Populated to the ids the call sites push.
    enum class StyleColorId : u32
    {
        Text,
        Button,
        FrameBg,
    };

    enum class StyleVarId : u32
    {
        WindowPadding,
        FramePadding,
        ItemSpacing,
    };
}
