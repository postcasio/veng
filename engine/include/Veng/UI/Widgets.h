#pragma once
#include <Veng/Veng.h>
#include <Veng/UI/Types.h>
#include <Veng/ImGui/ImGuiTexture.h>
#include <span>

// Veng::UI immediate-mode widget vocabulary. The widgets are overloaded on the
// value type and configured through the options structs in Types.h rather than
// raw ImGui flags. Every editable widget keeps immediate-mode semantics: it
// returns a [[nodiscard]] bool meaning "changed this frame". Text is preformatted
// (a string_view), so a caller writes UI::Text(fmt::format(...)) — no printf
// varargs. Signatures name only engine types; <imgui.h> appears only in the .cpp.

namespace Veng::UI
{
    // Edits — one Drag overloaded across the value type absorbs the whole
    // DragFloat/DragFloat2/3/4 + DragInt family.
    [[nodiscard]] bool Drag(string_view label, f32& v, DragOptions options = {});
    [[nodiscard]] bool Drag(string_view label, vec2& v, DragOptions options = {});
    [[nodiscard]] bool Drag(string_view label, vec3& v, DragOptions options = {});
    [[nodiscard]] bool Drag(string_view label, vec4& v, DragOptions options = {});
    [[nodiscard]] bool Drag(string_view label, i32& v, DragOptions options = {});

    [[nodiscard]] bool Slider(string_view label, f32& v, SliderOptions options);
    [[nodiscard]] bool Slider(string_view label, i32& v, i32 min, i32 max);

    [[nodiscard]] bool Checkbox(string_view label, bool& v);

    // Commits on deactivate, owning an internal scratch buffer: it edits the
    // scratch and writes v back only on Enter or deactivate-after-edit. The
    // returned bool means committed (a new value was written to v), not "edited
    // this frame" — so it is not paired with UI::ItemEdited().
    [[nodiscard]] bool InputText(string_view label, string& v);

    [[nodiscard]] bool Combo(string_view label, i32& index, std::span<const string_view> items);

    // Buttons / selection.
    [[nodiscard]] bool Button(string_view label);
    [[nodiscard]] bool Selectable(string_view label, bool selected = false);

    // Text — preformatted, no varargs.
    void Text(string_view text);
    void TextDisabled(string_view text);
    void TextColored(vec4 color, string_view text);
    void Label(string_view label, string_view value);

    // A registered ImGui texture (the engine's own wrapper, not a raw imgui type).
    void Image(const Ref<ImGuiTexture>& tex, vec2 size);
}
