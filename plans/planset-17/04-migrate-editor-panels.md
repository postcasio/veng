# Plan 04 — the editor panels on `Veng::UI`

**Goal:** migrate the remaining editor widget-authoring `ImGui::` call sites onto
`Veng::UI`, completing the editor side of the toolkit. After this plan the only raw
`ImGui::` left in the editor is the **frame-lifecycle / dock / present plumbing** in
`EditorHost` (and `ImGuiLayer` in the engine) — the deliberate integration-layer boundary
(planset decision 4).

**Depends on** plans 00 (widgets) and 01 (scopes). Independent of plan 03 (different files).

## The call sites

The remaining widget-authoring sites, by file (counts from the survey):

| File | Sites | Notable calls |
|---|---|---|
| `editor/src/panels/MaterialEditorPanel.cpp` | 41 | imnodes canvas chrome, node-property inspector, toolbar buttons, combos |
| `editor/src/panels/AssetBrowserPanel.cpp` | 16 | `Selectable` asset list, `BeginChild` scroll region, double-click open, context popup |
| `editor/src/panels/TextureEditorPanel.cpp` | 14 | `Image` preview, `Checkbox`/`Combo` settings, `Button` save |
| `editor/src/EditorHost.cpp` | 13 | menu bar (`BeginMainMenuBar`/`Menu`/`MenuItem`) — **widget part migrates**; dock/present **stays raw** |
| `editor/src/panels/ConsolePanel.cpp` | 8 | `TextColored` log lines, `BeginChild` + `SetScrollHereY` autoscroll, `Combo` filter |
| `editor/src/panels/InspectorPanel.cpp` | 4 | `Text` headers, `Separator`, component sections |
| `editor/src/panels/SceneViewportPanel.cpp` | 2 | `Image` scene texture, `ContentRegionAvail` |
| `editor/src/material/MaterialPreview.cpp` | 1 | `Image` preview |

### `EditorHost.cpp` — the split

`EditorHost` is the one file that holds **both** sides of the boundary:

- **Migrates (widget authoring):** the menu bar — `BeginMainMenuBar`/`BeginMenu`/`MenuItem`/
  `EndMenu`/`EndMainMenuBar` (`EditorHost.cpp:352–371`) → `if (auto bar = UI::MainMenuBar())
  { if (auto m = UI::Menu("File")) { if (UI::MenuItem("Exit")) … } … }`, with the
  Window-menu panel toggles using `UI::MenuItem(title, &slot.Open)`. The per-panel
  `Begin`/`End` + the no-padding `PushStyleVar`/`PopStyleVar` (`:398–416`) — already moved to
  `UI::Window` + `UI::StyleVar` in plan 01's `GetWindowFlags` retype, confirmed here.
- **Stays raw (host plumbing, planset decision 4):** `DockSpaceOverViewport`,
  `GetMainViewport`, `UpdatePlatformWindows`, `RenderPlatformWindowsDefault`, and the
  `EditorHost::BuildPresentGraph()` ImGui-only swapchain blit. These are the integration
  layer; `Veng::UI` does not wrap them.

## Decisions

1. **imnodes stays raw in `MaterialEditorPanel`.** The node *canvas* is imnodes
   (`ImNodes::`), editor-private, already linked PRIVATE — it is **not** an `ImGui::`
   widget-authoring site and is out of `Veng::UI`'s scope (the design overview: "the imnodes
   node canvas stays editor-private, exactly as imnodes already is"). What migrates in
   `MaterialEditorPanel` is the **`ImGui::` chrome around** the canvas — the toolbar buttons,
   the node-property inspector (which already calls `DrawFieldWidget`, migrated in plan 03),
   the combos, and the `Begin`/`End`/`PushID` pairs. The `ImNodes::` calls are left
   untouched.

2. **`ConsolePanel` autoscroll: `BeginChild` scope + `UI::ScrollToHere`.** The
   `BeginChild`/`EndChild` scroll region becomes
   `if (auto c = UI::Child("log", {}, UI::WindowFlags::HorizontalScrollbar))`, the per-line
   `TextColored` becomes `UI::TextColored(color, line)` with the level color as a `vec4`, and
   the `SetScrollHereY` becomes `UI::ScrollToHere()`. The
   `ImGuiWindowFlags_HorizontalScrollbar` on the child re-expresses through the
   `UI::WindowFlags::HorizontalScrollbar` enumerator and the `UI::Child(id, size,
   WindowFlags)` overload — both landed in plan 01 (its decision 6), so no enum change is
   needed here.

3. **`AssetBrowserPanel` list + context popup.** The asset rows become `UI::Selectable`;
   the `BeginChild` scroll region a `UI::Child` scope; the double-click open uses the
   thin mouse query left raw (planset decision 5 — `IsMouseDoubleClicked` overlaps the
   event area and is **not** wrapped this round; the one site stays raw with a comment
   flagging the area-4 convergence). The right-click context popup becomes
   `UI::OpenPopup`/`if (auto p = UI::Popup(id))`.

4. **`TextureEditorPanel` / `SceneViewportPanel` / `MaterialPreview` previews use
   `UI::Image`** over the `Ref<ImGuiTexture>` form (the same accessor pattern plan 02 adds
   for the composite pass — these panels already hold their preview texture as a `Ref`, so
   they pass it directly). The settings widgets (`Checkbox` sRGB, `Combo` filter/wrap,
   `Button` save) map one-to-one.

5. **The raw key/mouse-query sites stay raw, flagged.** `IsKeyPressed`,
   `IsMouseClicked`, `IsMouseDoubleClicked`, `GetMousePosOnOpeningCurrentPopup` (the
   handful across the panels) are **not** wrapped (planset decision 5). Each surviving site
   gets a one-line comment that it converges with the event/input area (area 4), so the
   leftover raw `ImGui::` is intentional and accounted for, not an oversight.

## Files

| File | Change |
|---|---|
| `editor/src/EditorHost.cpp` | Menu bar → `UI::MainMenuBar`/`Menu`/`MenuItem`; confirm the plan-01 `Window`/`StyleVar` migration; dock/present stay raw. |
| `editor/src/panels/MaterialEditorPanel.cpp` | `ImGui::` chrome → `Veng::UI`; `ImNodes::` canvas untouched. |
| `editor/src/panels/AssetBrowserPanel.cpp` | `Selectable`/`Child`/popup → `Veng::UI`; double-click query stays raw (flagged). |
| `editor/src/panels/TextureEditorPanel.cpp` | Preview `Image` + settings widgets → `Veng::UI`. |
| `editor/src/panels/ConsolePanel.cpp` | `Child`/`TextColored`/`ScrollToHere`/`Combo` → `Veng::UI`. |
| `editor/src/panels/InspectorPanel.cpp` | `Text`/`Separator`/sections → `Veng::UI`. |
| `editor/src/panels/SceneViewportPanel.cpp` | `Image`/`ContentRegionAvail` → `Veng::UI`. |
| `editor/src/material/MaterialPreview.cpp` | Preview `Image` → `Veng::UI`. |

No `Veng/UI/` header changes here — `WindowFlags::HorizontalScrollbar` and the
`UI::Child(id, size, WindowFlags)` overload the console child needs both land in plan 01.

Each `.cpp` adds `#include <Veng/UI/UI.h>` and drops `#include <imgui.h>` where the
remaining raw sites (decision 5) no longer need it — files with a flagged key/mouse-query
site keep the imgui include with a comment.

## Verification

- Clean build — every editor panel compiles against `Veng::UI`.
- The editor exe runs end-to-end: `hello_triangle-editor` launches, the menu bar
  (File→Exit, Window→panel toggles) works, the asset browser lists + opens assets, the
  texture editor previews + edits + saves, the console logs + autoscrolls, the material
  editor's node canvas (imnodes) draws with migrated chrome, and the scene viewport shows
  the scene. No visual or behavioral regression.
- `grep -rn "ImGui::" editor/` returns **only**: `EditorHost`'s frame-lifecycle/dock/present
  plumbing, `ImGuiLayer` (engine, not editor), and the decision-5 flagged key/mouse-query
  sites — each accounted for. No widget-authoring `ImGui::` remains.
- `ctest` green; `include_hygiene` unaffected (editor src, not public headers).
