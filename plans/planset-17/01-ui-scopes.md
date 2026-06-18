# Plan 01 — `Veng::UI` RAII scopes + flag enums

**Goal:** land the RAII scope guards — the half of `Veng::UI` that replaces the manual
`Begin`/`End`, `PushID`/`PopID`, and push/pop-style pairs with guards that close on scope
exit, surviving early returns — plus the vocab flag enums they consume, and retype the one
public-header `ImGui*` leak (`EditorPanel::GetWindowFlags()`).

## What lands

### `Veng/UI/Types.h` — flag/style vocab enums (extends plan 00's file)

Small engine enums, **not** `ImGui*Flags` (planset decision 1). Bitwise-combinable where
ImGui's are.

```cpp
namespace Veng::UI
{
    // Only the window/child knobs the call sites actually use — was EditorPanel's
    // GetWindowFlags() -> ImGuiWindowFlags. Bitwise-combinable. Doubles as the flag set
    // UI::Child accepts (HorizontalScrollbar is a child knob — ConsolePanel's log region).
    enum class WindowFlags : u32 { None = 0, NoScrollbar = 1, NoScrollWithMouse = 2,
                                   HorizontalScrollbar = 4 };

    enum class TreeFlags : u32 { None = 0, DefaultOpen = 1, SpanAvailWidth = 2 };

    // The style ids the editor actually pushes — closed engine enums mapping to
    // ImGuiCol_ / ImGuiStyleVar_ in the .cpp.
    enum class StyleColorId : u32 { Text, Button, FrameBg, /* …as call sites need */ };
    enum class StyleVarId   : u32 { WindowPadding, FramePadding, ItemSpacing, /* … */ };
}
```

`WindowFlags`/`TreeFlags` get the usual `operator|`/`operator&` free functions (the `u32`
underlying type makes them trivial). The enumerators are populated to exactly what the
current call sites use — `NoScrollbar`/`NoScrollWithMouse` (`SceneViewportPanel`),
`HorizontalScrollbar` (`ConsolePanel`'s child log region — used by `UI::Child`, see
decision 6), `SpanAvailWidth` (`FieldWidget` tree nodes), `WindowPadding` (`EditorHost`'s
no-padding viewport, a `StyleVarId`) — not a speculative full mirror. (A later migration that
needs another flag — `MenuBar`, `NoCollapse`, … — adds the one enumerator with it; none of
the current sites use them, so none are added speculatively.)

### `Veng/UI/Scopes.h` — RAII for every begin/end + push/pop pair

Each guard is `[[nodiscard]]`, has an `explicit operator bool` (open → draw the body), and
ends in its destructor. The asymmetry is encapsulated: windows/child/table **always** call
their `End`; tree/collapsing-header call `TreePop` **only when open** — so the call site
never gets it wrong.

```cpp
namespace Veng::UI
{
    // if (auto w = UI::Window("Inspector")) { ... }   // End() on scope exit
    [[nodiscard]] ScopedWindow Window(string_view title, bool* open = nullptr,
                                      WindowFlags = WindowFlags::None);
    [[nodiscard]] ScopedChild  Child(string_view id, vec2 size = {},
                                     WindowFlags = WindowFlags::None);

    [[nodiscard]] ScopedTree   TreeNode(string_view label, TreeFlags = TreeFlags::None);
    [[nodiscard]] ScopedTree   CollapsingHeader(string_view label, TreeFlags = TreeFlags::None);

    // tables: scope owns Begin/EndTable; the per-row/col calls are free funcs
    [[nodiscard]] ScopedTable  Table(string_view id, i32 columns);
    void TableSetupColumn(string_view label);
    void TableHeadersRow();
    void TableNextRow();
    void TableNextColumn();
    void TableSetColumnIndex(i32 column);

    [[nodiscard]] ScopedMenuBar MainMenuBar();
    [[nodiscard]] ScopedMenu    Menu(string_view label);
    bool MenuItem(string_view label, bool enabled = true);
    bool MenuItem(string_view label, bool* selected);              // the Window-menu toggle form
    [[nodiscard]] ScopedPopup   Popup(string_view id);
    void OpenPopup(string_view id);

    // unconditional scopes — body always runs, dtor always pops
    [[nodiscard]] DisabledScope   Disabled(bool disabled = true);
    [[nodiscard]] IdScope         PushId(string_view id);
    [[nodiscard]] StyleColorScope StyleColor(StyleColorId, vec4);
    [[nodiscard]] StyleVarScope   StyleVar(StyleVarId, vec2);
    [[nodiscard]] StyleVarScope   StyleVar(StyleVarId, f32);
}
```

## Decisions

1. **The guards store a `bool`, not an imgui handle, and define their destructors
   out-of-line in `Scopes.cpp`** (planset decision 2). `ScopedWindow` holds `bool m_Open`
   and `~ScopedWindow()` calls `ImGui::End()` unconditionally; `ScopedTree` holds
   `bool m_Open` and `~ScopedTree()` calls `ImGui::TreePop()` only if `m_Open`. Because the
   destructor is defined in the `.cpp`, `Scopes.h` needs no `<imgui.h>` — the guard types
   are plain structs with a `bool` member, a deleted copy, an `explicit operator bool`, and
   a declared (not defined) destructor. The unconditional scopes (`DisabledScope`,
   `IdScope`, `StyleColorScope`, `StyleVarScope`) hold whatever pop-count they pushed and
   always pop in the dtor.

2. **Guards are move-only and non-copyable.** Each owns a one-shot end/pop; copying would
   double-end. They are returned by value from the factory functions (guaranteed-elision /
   move), held by `if (auto x = UI::Window(...))` or a plain local. Moving transfers the
   "must end" responsibility (the moved-from guard's `bool` is cleared so its dtor is a
   no-op).

3. **`MainMenuBar`/`Menu`/`Popup`/`Table`/`Child`/`Window` follow ImGui's own
   always-vs-conditional-End rules**, encapsulated per guard: `BeginMainMenuBar` /
   `BeginMenu` / `BeginPopup` / `BeginTable` / `BeginCombo` are conditional (`End*` only
   when the begin returned true — the guard's dtor checks `m_Open`); `Begin` (window) and
   `BeginChild` are unconditional (`End`/`EndChild` always). The guard for each kind bakes
   in the correct rule so a call site cannot pair them wrong — the bug class this whole tier
   exists to kill.

4. **`MenuItem` stays a free function** (it has no begin/end), with two overloads: the
   plain action form (`MenuItem(label, enabled)` → bool clicked) and the toggle form
   (`MenuItem(label, bool* selected)`, the Window-menu panel-visibility checkboxes in
   `EditorHost`). `TableSetupColumn`/`TableHeadersRow`/`TableNextRow`/`TableNextColumn`/
   `TableSetColumnIndex` are likewise free (the row/column cursor is not a scope).

5. **`EditorPanel::GetWindowFlags()` is retyped here.** Its return changes from
   `ImGuiWindowFlags` to `UI::WindowFlags`, and the two overrides + the one caller move
   with it:
   - `editor/include/VengEditor/EditorPanel.h` — `virtual UI::WindowFlags GetWindowFlags()
     const { return UI::WindowFlags::None; }`.
   - `editor/src/panels/SceneViewportPanel.h` — `return UI::WindowFlags::NoScrollbar |
     UI::WindowFlags::NoScrollWithMouse;`.
   - `editor/src/EditorHost.cpp` (`BuildDockSpace`/panel loop) — reads the engine flags and
     passes them to `UI::Window` (or, while EditorHost's per-panel `Begin` stays raw until
     plan 04, maps them back through a `Veng::UI`-internal `ToImGui(WindowFlags)` the editor
     can call). To keep this plan self-contained, the `EditorHost` `Begin` call is migrated
     to `UI::Window` **here** as part of the retype (it is the natural consumer of
     `GetWindowFlags()`); the rest of `EditorHost`'s menu bar moves in plan 04. The
     `NoScrollbar`-implies-no-padding check (`EditorHost.cpp:408–413`) re-expresses
     `(flags & WindowFlags::NoScrollbar)` against the engine enum. The current code pushes
     `WindowPadding` **conditionally** (only when `noPadding`) and pops it right after `Begin`
     (`EditorHost.cpp:410–413`); the migration keeps it conditional by constructing the
     `UI::StyleVar` guard only in that branch (`optional<StyleVarScope>` or an equivalent
     guarded scope) — `WindowPadding` is read at `Begin`, so the guard outliving `Begin` into
     the window body is functionally identical to the current pop-immediately-after-`Begin`.

   This is the **first public-header `ImGui*` type leak `Veng::UI` removes**, even under
   the wrapper-only scope.

6. **`UI::Child` takes a `WindowFlags`.** `BeginChild` accepts window flags —
   `ConsolePanel`'s log region passes `HorizontalScrollbar` (`ConsolePanel.cpp:33–34`) — so
   `Child(id, size, WindowFlags)` carries them through the engine enum rather than dropping
   the capability. `HorizontalScrollbar` is one of the `WindowFlags` enumerators landed in
   this plan (above), so plan 04's `ConsolePanel` migration consumes it with no further enum
   change.

## Files

| File | Change |
|---|---|
| `engine/include/Veng/UI/Types.h` | Add `WindowFlags`/`TreeFlags`/`StyleColorId`/`StyleVarId` + bitwise ops. |
| `engine/include/Veng/UI/Scopes.h` | New — the guard types + factory functions + free table/menu helpers. |
| `engine/include/Veng/UI/UI.h` | Add `#include "Scopes.h"`. |
| `engine/src/UI/Scopes.cpp` | New — guard dtors + factories + enum→ImGui mapping (`#include <imgui.h>`). |
| `engine/CMakeLists.txt` | Add `src/UI/Scopes.cpp`. |
| `tests/include_hygiene/*` | Add `Veng/UI/Scopes.h` to the header set. |
| `editor/include/VengEditor/EditorPanel.h` | `GetWindowFlags()` → `UI::WindowFlags`. |
| `editor/src/panels/SceneViewportPanel.h` | Override returns `UI::WindowFlags`. |
| `editor/src/EditorHost.cpp` | Consume `UI::WindowFlags`; per-panel `Begin` → `UI::Window`. |

## Verification

- Clean build — the editor links against the retyped `GetWindowFlags()` and the new
  scopes.
- `include_hygiene` green — `Scopes.h` stays imgui-free (decision 1: `bool`-only members,
  out-of-line dtors). This is the load-bearing check for decision 2.
- `ctest` green; the `gpu`-band `hello_triangle_launcher_smoke` + `smoke_golden` unaffected
  (no rendered-pixel change).
- The editor exe builds and runs (manual: `hello_triangle-editor` opens, panels dock, the
  scene viewport's `NoScrollbar` behavior is preserved through the engine flag).
