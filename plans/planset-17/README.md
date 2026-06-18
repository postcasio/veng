# planset-17 — the `Veng::UI` toolkit

**Phase goal:** front ImGui with a thin, opinionated, engine-tier **`Veng::UI`**
vocabulary so UI is authored against an engine surface — consistent, overload-driven,
and free of raw `ImGui::` at the call site. Takes up
[future area 12](../future/README.md#12-ui-toolkit--vengui); the full design overview
is [future/ui-toolkit.md](../future/ui-toolkit.md).

ImGui is the one library veng consumes raw. Vulkan, GLFW, and VMA all sit behind an
engine vocabulary (`Renderer::Format`, the Native idiom); ImGui does not. **`ImGui::`
appears in 17 files; ~11 of them author widgets** (the rest name it only in comments —
`CommandBuffer.h`, `ImGuiTexture.h`, `ImGuiCompositePass.h`). Each widget-authoring site
picks its own widget, drag speed, slider flag,
format string, and ID discipline — there is no single place to make all scalar edits
behave the same, the `DragFloat`/`DragFloat2`/`DragFloat3` surface is multiplied into
the reflection inspector's hand-written per-type dispatch, and the `Begin`/`End`,
`PushID`/`PopID`, push/pop-style pairs are manual and must survive every early-out (the
bug class adjacent to the recent ImGuiLayer deferred-removal fixes).

`Veng::UI` is a **base immediate-mode vocabulary** in `libveng`
(`engine/include/Veng/UI/`, namespace `Veng::UI`) — overloaded on the value type,
configured through designated-initializer options structs (veng's `XInfo` idiom) rather
than ImGui flags, with RAII scope guards for every begin/end and push/pop pair. Every
editable widget keeps immediate-mode semantics: it returns `[[nodiscard]] bool`
"changed". Then every widget-authoring `ImGui::` call site in the tree — the
hello-triangle game module's debug panel and the editor's panels + reflection inspector
— migrates onto it.

## Scope decision — engine-tier, wrapper-only

Two decisions from the design overview fix the boundary of this work:

- **Engine-tier.** The base vocabulary lives in **`libveng`**, not the editor. Two
  consumers author UI: the editor (the bulk) **and game modules** (hello-triangle's
  debug panel calls `ImGui::` directly today). A wrapper that only lived in
  `libveng_editor` would force a game to link the whole editor framework for a debug
  slider. The engine tier serves both.
- **Wrapper-only.** ImGui **stays a PUBLIC dependency** of `libveng` this round. The aim
  is call-site consistency and a tight surface, *not* hiding ImGui. Driving ImGui fully
  private (the Native-idiom end-state — no `<imgui.h>` reachable through any public
  header, imgui linked PRIVATE, guarded by `include_hygiene`) is a **possible later
  planset**, explicitly out of scope here. `Veng::UI` is the prerequisite for it either
  way — and this planset takes a free step toward it (decision 2 below).

## Two tiers

| Tier | Lives in | Namespace | Contents |
|---|---|---|---|
| **Base immediate-mode vocab** | `libveng` (`Veng/UI/`) | `Veng::UI` | `Drag`/`Slider`/`Checkbox`/`Button`/`Text`/`Label`, layout helpers, RAII scopes. Replaces raw `ImGui::` at call sites. |
| **Stateful / editor widgets** | `libveng_editor` | `VengEditor` | The reflection inspector, asset picker, node canvas — built on `Veng::UI`. |

## Cross-cutting decisions

1. **One `Drag`, overloaded on the value type.** `Drag(label, f32&)`,
   `Drag(label, vec2&)`, `…vec3&`, `…vec4&`, `…i32&`, each taking a `DragOptions`
   designated-initializer struct (`{ .Speed, .Min, .Max, .Format }`). This collapses
   `DragFloat`/`DragFloat2`/`DragFloat3`/`DragFloat4`/`DragInt` into one name and lets
   the reflection inspector's `FieldClass::Vector` three-way `if` over
   `vec2`/`vec3`/`vec4` become a single overloaded call. The closed `FieldClass` set maps
   cleanly onto a closed `Veng::UI` overload set. Configurability is **deliberately
   reduced** for consistency — only the knobs the engine wants UI authors to vary are
   exposed; no `ImGui*Flags` value is ever a parameter.

2. **The `Veng/UI/` headers are imgui-free in their signatures and members** — even
   though imgui stays PUBLIC-linked. Options/flag enums are engine types
   (`DragOptions`, `WindowFlags`, `StyleColorId`); the RAII scope guards store a plain
   `bool` open-state and define their `ImGui::End`/`TreePop` destructors **out-of-line in
   the `.cpp`**. `<imgui.h>` appears only under `engine/src/UI/`. This is stricter than
   the wrapper-only floor requires, costs nothing, keeps the `Veng::UI` surface within
   `include_hygiene`'s existing guarantee, and leaves the headers already-clean for the
   future "drive imgui private" planset. The one ImGui-adjacent type a `Veng::UI`
   signature names is the **engine's own** `ImGuiTexture` (`UI::Image(const
   Ref<ImGuiTexture>&, vec2)`), which is already an engine wrapper, not a raw imgui type.

3. **Text takes a preformatted `string_view`, not printf varargs.** fmt is veng's
   formatting story, so a caller writes `UI::Text(fmt::format("{}: {}", a, b))`. This
   drops the printf foot-gun (`ImGui::LabelText(l, "%d", n)` → `UI::Label(l,
   fmt::format("{}", n))`) and keeps one formatting idiom across the codebase.

4. **The ImGui frame lifecycle and host plumbing stay raw.** `CreateContext` /
   `NewFrame` / `Render` / `GetDrawData` / `GetIO` / `DockSpaceOverViewport` /
   `UpdatePlatformWindows` / … are the integration layer, not a widget-authoring surface,
   and wrapping them buys nothing. They stay confined to their existing homes
   (`ImGuiLayer`, `EditorHost`'s dock/present plumbing). This is the one place `ImGui::`
   remains an expected spelling; `Veng::UI` replaces `ImGui::` only at **authoring**
   sites.

5. **Keyboard/mouse queries are left intentionally thin.** `IsKeyPressed`,
   `IsMouseClicked`, … overlap the still-stubbed event/input area
   ([area 4](../future/README.md#4-event--input-systems)) and converge there rather than
   growing a parallel key-enum in `Veng::UI`. This planset wraps only the item/hover
   queries a widget author needs inline (`ItemHovered`, `ItemEdited`, `Tooltip`) and a
   thin `FrameRate()` for the stats readout; the raw key/mouse-button sites in the editor
   panels stay raw, flagged to converge with area 4.

6. **Phases 1 + 2 only; stateful editor widget *classes* are deferred.** The design
   overview's phase 3 ("stateful editor widgets — the file browser first") has **no
   existing instance to extract** — the editor has no in-ImGui file browser (it uses
   panel classes, and there is no nfd surface in `libveng_editor`), and its one shared
   editor-tier widget (`DrawFieldWidget` / `DrawAssetPicker`) is **stateless** and is
   migrated in place onto `Veng::UI` in phase 2. The "stateful widget whose `Draw()`
   returns an event" pattern (the `FileBrowser` example) becomes a named follow-on, taken
   up when a widget that actually holds persistent state (cwd, selection, filter, scroll)
   needs extracting. This planset delivers the base vocab and the **complete** migration
   of every widget-authoring `ImGui::` site across the engine sample and the editor.

## Surface

The base tier is six small headers under `engine/include/Veng/UI/`:

```
engine/include/Veng/UI/
  UI.h        — umbrella include (pulls the rest)
  Types.h     — vocab enums, options structs, no imgui types
  Widgets.h   — Drag/Slider/Checkbox/Button/Text/Label/Combo/InputText/Image/Selectable
  Layout.h    — Separator/SameLine/Spacing/ContentRegionAvail/ScrollToHere
  Scopes.h    — RAII: Window/Child/Tree/CollapsingHeader/Table/Menu/Popup/Disabled/Id/Style/Tooltip
  Query.h     — ItemHovered/ItemEdited/Tooltip/FrameRate
```

`engine/src/UI/{Widgets,Scopes,Layout,Query}.cpp` hold the implementations (the only
TUs that `#include <imgui.h>` for the wrapper). The grounded migration mapping —
representative, every distinct `ImGui::` widget-authoring call maps to one `Veng::UI`
spelling:

| Today | `Veng::UI` |
|---|---|
| `ImGui::DragFloat3(l, value_ptr(v), 0.5f)` | `UI::Drag(l, v, { .Speed = 0.5f })` |
| `ImGui::LabelText(l, "%d", n)` | `UI::Label(l, fmt::format("{}", n))` |
| `ImGui::Begin(t); … ; ImGui::End();` | `if (auto w = UI::Window(t)) { … }` |
| `ImGui::TreeNodeEx(l, …)` / `TreePop()` | `if (auto t = UI::TreeNode(l)) { … }` |
| `ImGui::PushID(l); … ; ImGui::PopID();` | `auto id = UI::PushId(l); …` |
| `ImGui::Image(tex->GetTextureId(), {w,h})` | `UI::Image(tex, {w, h})` |
| `ImGui::BeginMainMenuBar()` / `…Menu` / `MenuItem` | `if (auto bar = UI::MainMenuBar())` / `UI::Menu` / `UI::MenuItem` |

`EditorPanel::GetWindowFlags()` changes its return type from `ImGuiWindowFlags` to
`UI::WindowFlags` — the one public-header `ImGui*` type a consumer names today, and the
first leak `Veng::UI` removes even under the wrapper-only scope.

## Plans

| # | Plan | Summary | Status |
|---|---|---|---|
| 00 | [Base widget vocabulary](00-ui-base-vocab.md) | `Veng/UI/{UI,Types,Widgets,Layout,Query}.h` + `engine/src/UI/*.cpp`: the overloaded free functions (`Drag`/`Slider`/`Checkbox`/`Button`/`Selectable`/`Text`/`Label`/`InputText`/`Combo`/`Image`), layout helpers, and item/hover queries, plus the options structs. imgui-free signatures; added to `include_hygiene`. | done |
| 01 | [RAII scopes + flag enums](01-ui-scopes.md) | `Veng/UI/Scopes.h` + impl: `Window`/`Child`/`Tree`/`CollapsingHeader`/`Table`/`MainMenuBar`/`Menu`/`Popup`/`Disabled`/`PushId`/`StyleColor`/`StyleVar` guards (out-of-line `.cpp` dtors), the `WindowFlags`/`TreeFlags`/`StyleColorId`/`StyleVarId` vocab enums, and `EditorPanel::GetWindowFlags()` retyped to `UI::WindowFlags`. | done |
| 02 | [hello-triangle game-tier proof](02-migrate-hello-triangle.md) | Migrate `examples/hello-triangle/main.cpp`'s debug panel fully off raw `ImGui::` (Window/Stats panels, `Combo`, `Image`, `Text`, `ContentRegionAvail`, `FrameRate`). Proof: a game module authors UI with **zero** `ImGui::` at widget sites. | proposed |
| 03 | [Reflection inspector on `Veng::UI`](03-fieldwidget-rewrite.md) | Rewrite `editor/src/FieldWidget.cpp` (`DrawFieldWidget` + `DrawAssetPicker`) on `Veng::UI` overloads — the showcase: the `FieldClass::Vector` three-way switch collapses into one `Drag`, and the per-`FieldClass` widgets all route through the engine surface. Shared by the entity inspector and the node-property inspector. | proposed |
| 04 | [Editor panels on `Veng::UI`](04-migrate-editor-panels.md) | Migrate the remaining editor widget-authoring sites: `AssetBrowserPanel`, `ConsolePanel`, `InspectorPanel`, `MaterialEditorPanel`, `SceneViewportPanel`, `TextureEditorPanel`, `MaterialPreview`, and `EditorHost`'s menu bar + per-panel `Begin`/`End`. Frame-lifecycle/dock/present plumbing stays raw. | proposed |
| 05 | [Docs + roadmap re-cut](05-docs-roadmap.md) | `CLAUDE.md` `Veng::UI` paragraph, `plans/README.md` entry, `plans/future/README.md` area-12 status → done, this status table. No code. | proposed |

## Dependency analysis

```
00 (base vocab) ──┬─► 02 (hello-triangle proof)
                  ├─► 03 (FieldWidget rewrite)
                  └─► 04 (editor panels)
01 (scopes) ──────┘          │
                             ▼
                  05 (docs) — after all land
```

- **Plan 00** and **plan 01** together are the `Veng::UI` library. They are *mostly*
  independent (00 = free functions, 01 = scope guards + flag enums), but both add files
  under `engine/include/Veng/UI/` and `engine/src/UI/` and both edit the engine
  `CMakeLists.txt` SOURCES list and the `include_hygiene` header set — so landing them in
  **parallel worktrees** produces a trivial CMake/test-list merge. Running **00 → 01**
  inline avoids it. 01's only cross-library edit (the `EditorPanel::GetWindowFlags()`
  retype) is self-contained.
- **Plans 02 / 03 / 04** each depend on **both 00 and 01** (every migration uses both
  widgets and scopes). They are **mutually independent** — 02 touches only
  `examples/hello-triangle`, 03 only `editor/src/FieldWidget.cpp`, 04 the remaining
  `editor/src/panels/*` + `EditorHost.cpp` + `MaterialPreview` — sharing no source files,
  so they are safe to fan out to parallel worktrees once 00+01 have landed.
- **Plan 05** is docs-only and lands last (it documents what 00–04 delivered).

The natural order is **00 → 01 → {02, 03, 04} → 05**, with the middle band parallelizable.

## Process & conventions

Same cadence as every planset: implement → migrate any caller in the same pass → verify
(clean build, `ctest` green across the unit/death/cooker bands and the `gpu` band where a
device is present, smoke PPM correct size + exit 0) → update this table → one commit per
plan (`Plan NN: <summary>`, `Co-Authored-By` trailer).

Common to all plans:

- **`include_hygiene` stays green.** The `Veng/UI/` headers are backend-free and
  imgui-free in their signatures (decision 2); the test compiles every public header
  while linking only veng's PUBLIC deps, so a leaked backend include fails the build.
- **The smoke PPM stays correct size + exit 0.** No migration changes rendered pixels —
  `Veng::UI` translates to the identical ImGui calls. The `smoke_golden` capture is
  unaffected (the smoke path renders headless with no ImGui panels).
- **Contract comments are present-tense facts** — no "used to call ImGui directly"
  narrative; the comment policy in `CLAUDE.md` applies.
- **No `ImGui*Flags` value is ever a `Veng::UI` parameter** (decision 1). A migration
  site that passed a raw flag re-expresses it through the engine enum or the options
  struct.

> Status legend: `proposed` = drafted, awaiting review; `ready` = reviewed and approved;
> `done` = landed and verified.

## On completion

`libveng` ships an engine-tier `Veng::UI` immediate-mode vocabulary — overloaded `Drag`
absorbing the whole `DragFloat*`/`DragInt` family, options-struct configuration instead
of ImGui flags, RAII scope guards for every begin/end and push/pop pair, and `fmt`-based
preformatted text — with imgui-free public-header signatures. Every widget-authoring
`ImGui::` call site in the tree has migrated onto it: the hello-triangle game module's
debug panel authors UI with **zero** raw `ImGui::`, the reflection inspector's
per-`FieldClass` dispatch routes through one overloaded `Drag` (the `Vector` switch
gone), and the editor's panels + menu bar are on the engine surface. ImGui's frame
lifecycle and host/dock/present plumbing remain raw in `ImGuiLayer`/`EditorHost` — the
deliberate integration-layer boundary. ImGui stays PUBLIC-linked (wrapper-only); the
`Veng::UI` surface is the prerequisite a future "drive imgui private" planset builds on,
and the imgui-free headers already meet that planset's header contract. The stateful
editor-widget-class pattern (`FileBrowser`-style `Draw()`-returns-event) is the named
follow-on.
