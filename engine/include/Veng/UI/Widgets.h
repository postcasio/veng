#pragma once
#include <Veng/Veng.h>
#include <Veng/UI/Types.h>
#include <Veng/ImGui/ImGuiTexture.h>
#include <span>

/// @brief Immediate-mode widget vocabulary for `Veng::UI`.
///
/// Widgets are overloaded on value type and configured through the options structs in
/// `Types.h` rather than raw ImGui flags. Every editable widget returns `[[nodiscard]] bool`
/// meaning "changed this frame". Text is preformatted (`string_view`), so a caller writes
/// `UI::Text(fmt::format(...))` — no printf varargs. Signatures name only engine types;
/// `<imgui.h>` appears only in the `.cpp`.

namespace Veng::UI
{
    /// @brief Drag-edits a float value.
    /// @param label    Widget label and ImGui id.
    /// @param v        Value to edit in place.
    /// @param options  Speed, clamp bounds, and format string.
    /// @return True the frame the value changed.
    [[nodiscard]] bool Drag(string_view label, f32& v, DragOptions options = {});

    /// @brief Drag-edits a `vec2` value.
    /// @param label    Widget label and ImGui id.
    /// @param v        Value to edit in place.
    /// @param options  Speed, clamp bounds, and format string.
    /// @return True the frame the value changed.
    [[nodiscard]] bool Drag(string_view label, vec2& v, DragOptions options = {});

    /// @brief Drag-edits a `vec3` value.
    /// @param label    Widget label and ImGui id.
    /// @param v        Value to edit in place.
    /// @param options  Speed, clamp bounds, and format string.
    /// @return True the frame the value changed.
    [[nodiscard]] bool Drag(string_view label, vec3& v, DragOptions options = {});

    /// @brief Drag-edits a `vec4` value.
    /// @param label    Widget label and ImGui id.
    /// @param v        Value to edit in place.
    /// @param options  Speed, clamp bounds, and format string.
    /// @return True the frame the value changed.
    [[nodiscard]] bool Drag(string_view label, vec4& v, DragOptions options = {});

    /// @brief Drag-edits an integer value.
    /// @param label    Widget label and ImGui id.
    /// @param v        Value to edit in place.
    /// @param options  Speed, clamp bounds, and format string.
    /// @return True the frame the value changed.
    [[nodiscard]] bool Drag(string_view label, i32& v, DragOptions options = {});

    /// @brief Slider-edits a float value within the declared range.
    /// @param label    Widget label and ImGui id.
    /// @param v        Value to edit in place.
    /// @param options  Min, max, and format string (required — a slider without a range is meaningless).
    /// @return True the frame the value changed.
    [[nodiscard]] bool Slider(string_view label, f32& v, SliderOptions options);

    /// @brief Slider-edits an integer value within the declared range.
    /// @param label  Widget label and ImGui id.
    /// @param v      Value to edit in place.
    /// @param min    Lower bound.
    /// @param max    Upper bound.
    /// @return True the frame the value changed.
    [[nodiscard]] bool Slider(string_view label, i32& v, i32 min, i32 max);

    /// @brief Toggles a bool value with a checkbox.
    /// @param label  Widget label and ImGui id.
    /// @param v      Value to edit in place.
    /// @return True the frame the value changed.
    [[nodiscard]] bool Checkbox(string_view label, bool& v);

    /// @brief Edits a string value, committing on Enter or deactivate-after-edit.
    ///
    /// Owns an internal scratch buffer: edits the scratch and writes `v` back only on
    /// Enter or deactivate-after-edit. The returned bool means committed (a new value
    /// was written to `v`), not "edited this frame" — do not pair it with `UI::ItemEdited()`.
    /// @param label  Widget label and ImGui id.
    /// @param v      Value written back on commit.
    /// @return True the frame a new value was committed to `v`.
    [[nodiscard]] bool InputText(string_view label, string& v);

    /// @brief Dropdown that selects among a fixed list of string items.
    /// @param label  Widget label and ImGui id.
    /// @param index  Current selection index, updated in place.
    /// @param items  The selectable string items.
    /// @return True the frame the selection changed.
    [[nodiscard]] bool Combo(string_view label, i32& index, std::span<const string_view> items);

    /// @brief Draws a clickable button.
    /// @param label  Button label and ImGui id.
    /// @return True the frame the button is clicked.
    [[nodiscard]] bool Button(string_view label);

    /// @brief Draws a selectable row.
    /// @param label     Row label and ImGui id.
    /// @param selected  Whether the row appears highlighted.
    /// @return True the frame the row is clicked.
    [[nodiscard]] bool Selectable(string_view label, bool selected = false);

    /// @brief Draws preformatted text.
    /// @param text  Text to display.
    void Text(string_view text);

    /// @brief Draws preformatted text in a greyed-out style.
    /// @param text  Text to display.
    void TextDisabled(string_view text);

    /// @brief Draws preformatted text in a specified color.
    /// @param color  RGBA text color.
    /// @param text   Text to display.
    void TextColored(vec4 color, string_view text);

    /// @brief Draws a label/value pair on the same row.
    /// @param label  Left-column label text.
    /// @param value  Right-column value text.
    void Label(string_view label, string_view value);

    /// @brief Draws a registered ImGui texture at the specified size.
    /// @param tex   Engine wrapper around the registered descriptor set.
    /// @param size  Display size in pixels.
    void Image(const Ref<ImGuiTexture>& tex, vec2 size);
}
