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
}
