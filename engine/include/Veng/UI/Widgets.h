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

    /// @brief Slider-edits a `vec2` value within the declared range.
    /// @param label    Widget label and ImGui id.
    /// @param v        Value to edit in place.
    /// @param options  Min, max, and format string (required — a slider without a range is meaningless).
    /// @return True the frame the value changed.
    [[nodiscard]] bool Slider(string_view label, vec2& v, SliderOptions options);

    /// @brief Slider-edits a `vec3` value within the declared range.
    /// @param label    Widget label and ImGui id.
    /// @param v        Value to edit in place.
    /// @param options  Min, max, and format string (required — a slider without a range is meaningless).
    /// @return True the frame the value changed.
    [[nodiscard]] bool Slider(string_view label, vec3& v, SliderOptions options);

    /// @brief Slider-edits a `vec4` value within the declared range.
    /// @param label    Widget label and ImGui id.
    /// @param v        Value to edit in place.
    /// @param options  Min, max, and format string (required — a slider without a range is meaningless).
    /// @return True the frame the value changed.
    [[nodiscard]] bool Slider(string_view label, vec4& v, SliderOptions options);

    /// @brief Slider-edits an integer value within the declared range.
    /// @param label  Widget label and ImGui id.
    /// @param v      Value to edit in place.
    /// @param min    Lower bound.
    /// @param max    Upper bound.
    /// @return True the frame the value changed.
    [[nodiscard]] bool Slider(string_view label, i32& v, i32 min, i32 max);

    /// @brief Edits an RGB color through a swatch + picker popup.
    /// @param label  Widget label and ImGui id.
    /// @param v      RGB color edited in place; each channel in [0, 1].
    /// @return True the frame the color changed.
    [[nodiscard]] bool ColorEdit3(string_view label, vec3& v);

    /// @brief Edits an RGBA color through a swatch + picker popup.
    /// @param label  Widget label and ImGui id.
    /// @param v      RGBA color edited in place; each channel in [0, 1].
    /// @return True the frame the color changed.
    [[nodiscard]] bool ColorEdit4(string_view label, vec4& v);

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

    /// @brief Edits a string value, showing greyed hint text while empty.
    ///
    /// Behaves exactly like `InputText` — the same internal scratch buffer, the same
    /// commit-on-Enter-or-deactivate semantics, the same "committed" return — but draws
    /// `hint` placeholder text whenever the field is empty. Used for rename fields and
    /// search boxes.
    /// @param label  Widget label and ImGui id.
    /// @param hint   Placeholder text shown while the field is empty.
    /// @param v      Value written back on commit.
    /// @return True the frame a new value was committed to `v`.
    [[nodiscard]] bool InputTextWithHint(string_view label, string_view hint, string& v);

    /// @brief Edits a string value in a multi-line box, committing on deactivate-after-edit.
    ///
    /// The multi-line sibling of `InputText`: it owns the same internal scratch buffer and
    /// writes `v` back only on deactivate-after-edit (Enter inserts a newline in a multi-line
    /// field rather than committing). The returned bool means committed, not "edited this
    /// frame".
    /// @param label  Widget label and ImGui id.
    /// @param v      Value written back on commit.
    /// @param size   Box size in pixels; a zero component takes ImGui's default extent.
    /// @return True the frame a new value was committed to `v`.
    [[nodiscard]] bool InputTextMultiline(string_view label, string& v, vec2 size = {});

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

    /// @brief Draws a compact, frame-padding-free button for per-row controls.
    /// @param label  Button label and ImGui id.
    /// @return True the frame the button is clicked.
    [[nodiscard]] bool SmallButton(string_view label);

    /// @brief Reserves an empty interactive rectangle of an exact size.
    ///
    /// Renders nothing, but registers an item with the given size — clickable, hoverable, and a
    /// valid drag/drop source/target anchor — with no frame padding or item-spacing expansion of
    /// its rectangle (unlike `Selectable`). The base for a custom-drawn widget whose content is
    /// overlaid afterward, where the item rect must match the drawn box exactly (so `ItemBorder`
    /// frames it tightly).
    /// @param id    ImGui id for the item.
    /// @param size  Exact rectangle size in pixels.
    /// @return True the frame the rectangle is clicked.
    [[nodiscard]] bool InvisibleButton(string_view id, vec2 size);

    /// @brief Draws a button that renders in the accent state while toggled on.
    ///
    /// When `active` is true the button fills with the theme `Accent` (hover/press use
    /// `AccentHovered`/`AccentActive`) so its on-state reads at a glance — a toolbar
    /// toggle. A click flips `active` in place. Returns true only the frame the state
    /// changes, matching the "changed" convention of every editable widget.
    /// @param label   Button label and ImGui id.
    /// @param active  Toggle state, flipped in place on click.
    /// @return True the frame `active` changes.
    [[nodiscard]] bool ToggleButton(string_view label, bool& active);

    /// @brief Draws a selectable row.
    ///
    /// `SelectableFlags::SpanAllColumns` makes the highlight cover a multi-column table
    /// row rather than the first column; `SelectableFlags::AllowDoubleClick` lets a caller
    /// pair the return with `IsMouseDoubleClicked` to act on a double-click.
    /// @param label     Row label and ImGui id.
    /// @param selected  Whether the row appears highlighted.
    /// @param flags     Selectable behavior flags.
    /// @return True the frame the row is clicked.
    [[nodiscard]] bool Selectable(string_view label, bool selected = false,
                                  SelectableFlags flags = SelectableFlags::None);

    /// @brief Draws a selectable of a fixed size.
    ///
    /// The sized variant: the highlight and hit area fill the requested rectangle rather
    /// than one text line, so a caller can overlay its own content (an icon-grid cell). A
    /// zero component fills the available width / takes the line height, matching ImGui.
    /// @param label     Row label and ImGui id.
    /// @param selected  Whether the cell appears highlighted.
    /// @param size      Hit-area size in pixels.
    /// @param flags     Selectable behavior flags.
    /// @return True the frame the cell is clicked.
    [[nodiscard]] bool Selectable(string_view label, bool selected, vec2 size,
                                  SelectableFlags flags = SelectableFlags::None);

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

    /// @brief Draws a type name with its namespace de-emphasized in parentheses, inline.
    ///
    /// Renders `name`, then — when `ns` is non-empty — `(ns)` in the disabled (greyed)
    /// style on the same line, e.g. `string (Veng)`. The canonical way to show a reflected
    /// type in editor UI.
    /// @param name  The bare type name.
    /// @param ns    The enclosing namespace; omitted from the output when empty.
    void TypeLabel(string_view name, string_view ns);

    /// @brief Formats a type name and namespace as a single "Name (Namespace)" string.
    ///
    /// For widgets that take one label string and so cannot de-emphasize part of it (combo
    /// items, selectable labels). Yields just `name` when `ns` is empty.
    /// @param name  The bare type name.
    /// @param ns    The enclosing namespace.
    /// @return The combined label.
    [[nodiscard]] string FormatTypeLabel(string_view name, string_view ns);

    /// @brief Draws a horizontal separator with a centered text label.
    ///
    /// A labeled section divider, used to head a group of inspector rows.
    /// @param text  Section label drawn within the separator.
    void SeparatorText(string_view text);

    /// @brief Draws a registered ImGui texture at the specified size.
    /// @param tex   Engine wrapper around the registered descriptor set.
    /// @param size  Display size in pixels.
    void Image(const Ref<ImGuiTexture>& tex, vec2 size);

    /// @brief Plots a line graph of a value series.
    ///
    /// The series is plotted left (oldest) to right (newest); `options.Offset` names the
    /// oldest sample so a ring buffer plots in order without being rotated first. The plot
    /// is non-interactive — a rolling readout, not an editable widget — so it returns void.
    /// @param label    Graph label and ImGui id.
    /// @param values   The value series to plot.
    /// @param options  Overlay text, axis scale, ring-buffer offset, and graph size.
    void PlotLines(string_view label, std::span<const f32> values, PlotOptions options = {});

    /// @brief Plots several colored value series as lines on one shared chart.
    ///
    /// Each series is drawn as a polyline in its own color over a common axis, so overlapping
    /// rolling histories (per-pass GPU cost, say) compare directly. The Y axis spans
    /// [ScaleMin, ScaleMax]; an unset ScaleMax autoscales to the largest sample across every
    /// series. Non-interactive, so it returns void. `options.Offset` is unused — each series
    /// carries its own ring-buffer offset.
    /// @param label    Chart ImGui id.
    /// @param series   The colored series to overlay.
    /// @param options  Overlay text, axis scale, and chart size.
    void PlotLinesMulti(string_view label, std::span<const PlotSeries> series,
                        PlotOptions options = {});

    /// @brief Strokes a border around the most recently submitted item's rectangle.
    ///
    /// Draws over the previous item without consuming layout space — an accent frame on a
    /// viewport image that has captured the mouse, for instance. Authored color is sRGB.
    /// @param color      Border color, authored sRGB (linearized for the UI pipeline).
    /// @param thickness  Stroke width in pixels.
    void ItemBorder(vec4 color, f32 thickness = 2.0f);

    /// @brief Draws a filled, rounded badge with centered glyph text.
    ///
    /// A status/type chip: a rounded-rect fill in `color` with `text` centered over it in
    /// the theme's on-accent color. With `size == {}` the badge auto-sizes to the text plus
    /// frame padding (an inline chip); an explicit `size` draws a fixed tile (an icon-grid
    /// glyph). Advances the cursor by the badge's size as a non-interactive layout item.
    /// @param color  Fill color, authored sRGB (linearized for the UI pipeline).
    /// @param text   Glyph text drawn centered over the fill.
    /// @param size   Fixed badge size in pixels; `{}` auto-sizes to the text.
    void Badge(string_view text, vec4 color, vec2 size = {});

    /// @brief Draws a horizontal progress bar filled with the theme accent.
    ///
    /// A non-negative `fraction` (clamped to `[0, 1]`) fills the bar left-to-right; a
    /// negative `fraction` draws an animated indeterminate sweep — work in progress with no
    /// measurable completion — so a caller with nothing to report passes any negative value.
    /// `overlay` is drawn centered over the bar; empty draws the default percentage for a
    /// determinate bar and nothing for an indeterminate one. Non-interactive, so it returns
    /// void.
    /// @param fraction  Completion in `[0, 1]`, or negative for an indeterminate animation.
    /// @param size      Bar size in pixels; a negative width fills the content region, a zero
    ///                  height takes the frame height.
    /// @param overlay   Text drawn centered over the bar, or empty for the default.
    void ProgressBar(f32 fraction, vec2 size = {-1.0f, 0.0f}, string_view overlay = {});
}
