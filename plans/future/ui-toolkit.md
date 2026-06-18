# UI toolkit — `Veng::UI` — design overview (future)

> **Vision / design sketch, not scheduled.** Addresses the ImGui sprawl: 17
> files call `ImGui::` directly today, each picking its own widget, flags, and
> spelling. The conclusion from design exploration: keep ImGui as the immediate-
> mode backend, but front it with a thin, opinionated **`Veng::UI`** vocabulary so
> UI is authored against an engine surface — consistent, overload-driven, and free
> of raw `ImGui::` at the call site.

## The problem

ImGui is the one library veng consumes raw. Vulkan, GLFW, and VMA all sit behind
an engine vocabulary (`Renderer::Format`, the Native idiom); ImGui does not.
Consequences:

- **No consistency knob.** Every call site chooses its own drag speed, slider
  flags, format string, and ID discipline. There is no single place to make all
  scalar edits behave the same.
- **1-to-1 surface.** `DragFloat` / `DragFloat2` / `DragFloat3` are distinct
  calls; the reflection inspector (`editor/src/FieldWidget.cpp`) already hand-
  writes the per-type dispatch this multiplies into
  (`FieldClass::Vector` → a three-way `if` over `vec2`/`vec3`/`vec4`).
- **Pairing footguns.** `Begin`/`End`, `PushID`/`PopID`, push/pop style are
  manual and must survive every early-out — the bug class adjacent to the
  ImGuiLayer deferred-removal fixes.

## Scope decision — engine-tier, wrapper-only

Two decisions fix the boundary of this work:

- **Engine-tier.** The base vocabulary lives in **`libveng`**
  (`engine/include/Veng/UI/`, namespace `Veng::UI`), not the editor. Two
  consumers author UI: the editor (the bulk) **and game modules** (hello-triangle's
  debug panel calls `ImGui::` directly today). A wrapper that only lived in
  `libveng_editor` would force a game to link the whole editor framework for a
  debug slider. The engine tier serves both.
- **Wrapper-only.** ImGui **stays a PUBLIC dependency** of `libveng` this round.
  The aim is call-site consistency and a tight surface, *not* hiding ImGui. So
  `<imgui.h>` may remain in public headers, imgui stays PUBLIC-linked, and the
  `include_hygiene` test is unchanged. Driving ImGui fully private (the Native-
  idiom end-state — no `<imgui.h>` in any public header, imgui linked PRIVATE,
  guarded by `include_hygiene`) is a **possible later planset**, explicitly out of
  scope here. `Veng::UI` is the prerequisite for it either way.

## Two tiers

| Tier | Lives in | Namespace | Contents |
|---|---|---|---|
| **Base immediate-mode vocab** | `libveng` (`Veng/UI/`) | `Veng::UI` | `Drag`/`Slider`/`Checkbox`/`Button`/`Text`/`Label`, layout helpers, RAII scopes. Replaces raw `ImGui::` at call sites. |
| **Stateful / editor widgets** | `libveng_editor` | `VengEditor` | File browser, the reflection inspector, asset picker, node canvas — built on `Veng::UI`. |

## Base tier — overloads + options structs

The widget set is **overloaded on the value type** and configured through
designated-initializer options structs (veng's `XInfo` idiom), not ImGui flags.
Immediate-mode semantics are kept: each editable widget returns `[[nodiscard]]
bool` "changed". The engine's own option enums/structs translate to ImGui flags
internally; no `ImGui*Flags` value is ever a parameter.

```cpp
namespace Veng::UI
{
    struct DragOptions  { f32 Speed = 0.01f; optional<f32> Min, Max; const char* Format = "%.3f"; };
    struct SliderOptions{ f32 Min = 0, Max = 1; const char* Format = "%.3f"; };

    bool Drag(string_view label, f32&  v, DragOptions = {});
    bool Drag(string_view label, vec2& v, DragOptions = {});   // was DragFloat2
    bool Drag(string_view label, vec3& v, DragOptions = {});   // was DragFloat3
    bool Drag(string_view label, vec4& v, DragOptions = {});
    bool Drag(string_view label, i32&  v, DragOptions = {});

    bool Slider(string_view label, f32& v, SliderOptions);
    bool Checkbox(string_view label, bool& v);
    bool Button(string_view label);
    void Text(string_view text);
    // …Label, InputText, Combo, etc.
}
```

This collapses the `FieldClass::Vector` switch into a single overloaded `Drag`,
and the closed `FieldClass` set maps cleanly onto a closed `Veng::UI` overload
set. Configurability is **deliberately reduced** for consistency — only the knobs
the engine wants UI authors to vary are exposed.

### RAII scopes

Begin/End-style pairs become scope guards that close on scope exit, surviving
early returns:

```cpp
if (auto win = UI::Window("Inspector")) { /* … */ }   // End() on scope exit
UI::IdScope id(label);                                  // PopID() on scope exit
```

## Editor tier — stateful widgets return events

Complex "all-in-one" widgets that hold persistent state (cwd, selection, filter,
scroll) are **classes**, not free functions. They stay immediate-mode-friendly:
`Draw()` is called every frame and **returns a result/event**, rather than
invoking a callback (callbacks tangle lifetimes and fight the single-thread,
immediate-mode model).

```cpp
class FileBrowser
{
public:
    explicit FileBrowser(FileBrowserOptions = {});
    optional<path> Draw();   // returns a path the frame the user confirms a pick
};

// in a panel:
if (auto picked = m_Browser.Draw())
    Open(*picked);
```

The existing `DrawAssetPicker` and the recursive `FieldWidget` walker are the same
species — editor-tier widgets rebuilt on `Veng::UI` primitives. The imnodes node
canvas stays editor-private, exactly as imnodes already is.

## Phasing (when taken up as a planset)

1. `Veng::UI` base vocab + RAII scopes in `libveng`; migrate hello-triangle's
   debug panel off raw `ImGui::` — proof a game module authors UI with no
   `ImGui::` call.
2. Migrate editor panels; rewrite `FieldWidget` on `Veng::UI` overloads.
3. Stateful editor widgets — the file browser first.

Hiding ImGui (PRIVATE link + `include_hygiene` guard) is **not** in these phases;
it is a separate later planset that `Veng::UI` unblocks.

## Surface sketch

Grounded in the current call sites — every distinct `ImGui::` call in the tree
falls into one of three buckets:

### What `Veng::UI` does *not* wrap — the backend boundary

ImGui **frame lifecycle and host plumbing** stay raw, confined to their existing
homes (`ImGuiLayer`, `EditorHost`) — they are the integration layer, not a widget
authoring surface, and wrapping them buys nothing:

`CreateContext` / `DestroyContext` / `NewFrame` / `EndFrame` / `Render` /
`GetDrawData` / `GetIO` / `GetStyle` / `GetMainViewport` / `DockSpaceOverViewport`
/ `UpdatePlatformWindows` / `RenderPlatformWindowsDefault`.

This is the one place `ImGui::` remains an expected spelling. Everything below
replaces a `ImGui::` call at an authoring site.

### Header layout

```
engine/include/Veng/UI/
  UI.h        — umbrella include (pulls the rest)
  Types.h     — vocab enums, options structs, no imgui types
  Widgets.h   — Drag/Slider/Checkbox/Button/Text/Combo/InputText/Image/Selectable
  Layout.h    — Separator/SameLine/Spacing/ContentRegionAvail + Child scope
  Scopes.h    — RAII: Window/TreeNode/Table/Menu/Popup/Disabled/Id/Style/Tooltip
  Query.h     — IsItemHovered/IsItemEdited + the thin mouse/key queries
```

### `Types.h` — vocabulary, no imgui

Options are designated-initializer structs (`XInfo` idiom); flags are small engine
enums, not `ImGui*Flags`. Colors are `vec4` (glm, already house vocab).

```cpp
namespace Veng::UI
{
    struct DragOptions   { f32 Speed = 0.01f; optional<f32> Min, Max; const char* Format = "%.3f"; };
    struct SliderOptions { f32 Min = 0.0f, Max = 1.0f; const char* Format = "%.3f"; };

    // Only the window knobs the editor actually varies — was EditorPanel's
    // GetWindowFlags() -> ImGuiWindowFlags. Bitwise-combinable.
    enum class WindowFlags : u32 { None = 0, NoScrollbar = 1, NoCollapse = 2, MenuBar = 4 };

    enum class TreeFlags  : u32 { None = 0, DefaultOpen = 1, SpanAvailWidth = 2, Framed = 4 };
}
```

### `Widgets.h` — overloaded, immediate-mode

One `Drag` overloaded on the value type; edits return `[[nodiscard]] bool`
"changed". **Text takes a preformatted `string_view`, not printf varargs** — fmt
is veng's formatting story, so a caller writes `UI::Text(fmt::format("{}:{}", a,
b))`; this drops the printf foot-gun and keeps one formatting idiom.

```cpp
namespace Veng::UI
{
    // edits — `Drag` absorbs DragFloat/DragFloat2/3/4 + DragInt
    bool Drag(string_view label, f32&  v, DragOptions = {});
    bool Drag(string_view label, vec2& v, DragOptions = {});
    bool Drag(string_view label, vec3& v, DragOptions = {});
    bool Drag(string_view label, vec4& v, DragOptions = {});
    bool Drag(string_view label, i32&  v, DragOptions = {});

    bool Slider(string_view label, f32& v, SliderOptions);
    bool Slider(string_view label, i32& v, i32 min, i32 max);

    bool Checkbox(string_view label, bool& v);
    bool InputText(string_view label, string& v);                 // owns the char-buffer dance
    bool Combo(string_view label, i32& index, span<const string_view> items);

    // buttons / selection
    bool Button(string_view label);
    bool Selectable(string_view label, bool selected = false);

    // text — preformatted, no varargs
    void Text(string_view text);
    void TextDisabled(string_view text);
    void TextColored(vec4 color, string_view text);
    void Label(string_view label, string_view value);             // was LabelText

    // a registered ImGui texture (the 16 ImGui::Image sites)
    void Image(const Ref<ImGuiTexture>& tex, vec2 size);
}
```

### `Scopes.h` — RAII for every begin/end + push/pop pair

Each scope is `explicit operator bool` (open → draw the body) and ends in its
destructor, surviving early returns. Windows/child/table **always** call their
`End`; tree/collapsing-header call `TreePop` **only when open** — the guard
encapsulates that asymmetry so the call site never gets it wrong.

```cpp
namespace Veng::UI
{
    // if (auto w = UI::Window("Inspector")) { ... }   // End() on scope exit
    [[nodiscard]] ScopedWindow Window(string_view title, bool* open = nullptr,
                                      WindowFlags = WindowFlags::None);
    [[nodiscard]] ScopedChild  Child(string_view id, vec2 size = {});

    [[nodiscard]] ScopedTree   TreeNode(string_view label, TreeFlags = TreeFlags::None);
    [[nodiscard]] ScopedTree   CollapsingHeader(string_view label, TreeFlags = TreeFlags::None);

    // tables: scope owns Begin/EndTable; the per-row/col calls are free funcs
    [[nodiscard]] ScopedTable  Table(string_view id, i32 columns);
    void TableSetupColumn(string_view label);
    void TableHeadersRow();
    void TableNextRow();
    void TableNextColumn();

    [[nodiscard]] ScopedMenuBar    MainMenuBar();
    [[nodiscard]] ScopedMenu       Menu(string_view label);
    bool MenuItem(string_view label, bool enabled = true);
    [[nodiscard]] ScopedPopup      Popup(string_view id);
    void OpenPopup(string_view id);

    // unconditional scopes — body always runs, dtor always pops
    [[nodiscard]] DisabledScope Disabled(bool disabled = true);
    [[nodiscard]] IdScope       PushId(string_view id);
    [[nodiscard]] StyleColorScope StyleColor(StyleColorId, vec4);
    [[nodiscard]] StyleVarScope   StyleVar(StyleVarId, f32);
}
```

### `Layout.h` / `Query.h` — the small remainder

```cpp
namespace Veng::UI
{
    void Separator();
    void SameLine();
    void Spacing();
    vec2 ContentRegionAvail();          // was GetContentRegionAvail
    void ScrollToHere();                // was SetScrollHereY

    bool ItemHovered();                 // was IsItemHovered
    bool ItemEdited();                  // was IsItemDeactivatedAfterEdit
    void Tooltip(string_view text);     // was the IsItemHovered + SetTooltip pair
}
```

Keyboard/mouse queries (`IsKeyPressed`, `IsMouseClicked`, …) are intentionally
left thin here — they overlap the still-stubbed event/input area, and converge
there rather than growing a parallel key-enum in `Veng::UI`.

### Migration mapping (representative)

| Today | `Veng::UI` |
|---|---|
| `ImGui::DragFloat3(l, value_ptr(v), 0.5f)` | `UI::Drag(l, v, { .Speed = 0.5f })` |
| `ImGui::LabelText(l, "%d", n)` | `UI::Label(l, fmt::format("{}", n))` |
| `ImGui::Begin(t); … ; ImGui::End();` | `if (auto w = UI::Window(t)) { … }` |
| `ImGui::TreeNodeEx(l, …)` / `TreePop()` | `if (auto t = UI::TreeNode(l)) { … }` |
| `ImGui::PushID(l); … ; ImGui::PopID();` | `auto id = UI::PushId(l); …` |
| `ImGui::Image(tex->GetTextureId(), {w,h})` | `UI::Image(tex, {w, h})` |

`EditorPanel::GetWindowFlags()` changes its return type from `ImGuiWindowFlags` to
`UI::WindowFlags` — the one public-header `ImGui*` type a consumer names today, and
the first leak `Veng::UI` removes even under the wrapper-only scope.
