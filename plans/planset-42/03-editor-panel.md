# Plan 03 — the input-mapping editor panel

**Goal:** the almost-free editor. Register an `InputMappingEditorPanel` for `AssetType::InputMap`
that draws the reflected context — the binding table through the existing `DrawFieldWidget`,
plus action-name labels — with the same live recook → hot-reload → preview loop the texture and
material editors use. Deliberately **basic**: the reflected inspector, not a bespoke
press-a-key-to-bind capture UX. Depends on Plan 02.

## The starting point

- The editor registers a panel per asset type. The **single-asset** editors — `TextureEditorPanel`
  (`AssetType::Texture`), `MaterialEditorPanel` (`AssetType::Material`) — derive from **`EditorPanel`**
  (the `AssetEditorPanel` dockspace + per-document `CommandStack` base is for the multi-child scene
  editors, `PrefabEditorPanel`/`LevelEditorPanel`; the single-asset editors have **no undo**). What
  they give the input-map panel is the pattern to copy: the cook-on-demand → hot-reload → preview
  loop.
- The **reflected-body-for-free** precedent is `ProjectSettingsPanel`/`LevelEditorPanel`, which draw a
  whole reflected data model — including a `FieldClass::Array` (`vector<BuildConfiguration>`) — through
  **`DrawFieldWidget`** with **no bespoke widget code** (the texture/material editors, by contrast,
  hand-roll their widgets). An `InputMappingContext` (`vector<InputAction>` + `vector<Binding>`) is the
  same shape as the `ProjectSettings` array, so it draws the same way.
- Cook-on-demand: `EditorHost::RequestCook` cooks one source via `TaskSystem` and hot-reloads the
  result behind the stable `AssetHandle` through `MountMemory`; the texture editor debounces edits
  and live-recooks. The `veng-editor` exe links `libveng_cook`.
- The reflection layer's `FieldDisplay` cascade (planset-36) gives named-enum combos
  (`device`/`kind`/`axis` draw as dropdowns) and per-field `DisplayName`/`Tooltip`/category grouping
  for free.

## What lands

### 1. `InputMappingEditorPanel`

A new editor panel (`editor/src/InputMappingEditorPanel.{h,cpp}`), registered for
`AssetType::InputMap`:

- **The document** is the loaded `InputMappingContext`'s source model — the `{ vector<InputAction>,
  vector<Binding> }` parsed from the `*.inputmap.json` (the editor edits the source, cooks it,
  previews the cooked result), preserving unknown JSON keys on save the way the texture/material
  editors do.
- **The body is `DrawFieldWidget` over the two arrays.** The `actions` list and the `bindings` list
  each draw as a reflected `FieldClass::Array` — add/remove rows, edit each field. `device`/`kind`/
  `axis` are `VE_ENUM` combos; `control`/`scale` are scalar widgets; `action`/`id` (an `ActionId`)
  use the registered `ActionId` combo below. This is the binding table with one custom widget — the
  "almost free" the panel is here to cash in.
- **Reflection metadata sharpens it cheaply.** `FieldDescriptor` `DisplayName`/`Tooltip`/`Category`
  on the `Binding`/`InputAction` fields (e.g. label `Control` "Key/Button code", group Source vs.
  Mapping) — authored on the structs in Plan 00/02, drawn free here. No panel code.
- **An `ActionId` needs a registered field widget — the one non-free piece.** `ActionId` is a `u64`
  leaf, and the generic scalar widget only handles `f32`/`i32`/`u32`/`bool`, drawing anything else as
  a disabled `(scalar)` ([editor/src/FieldWidget.cpp]). So a `Binding.Action` (and `InputAction.Id`)
  would be uneditable and unreadable through the free path. The panel therefore **registers a
  `RegisterFieldWidget(TypeIdOf<ActionId>(), …)`** — a small combo over the document's declared
  `actions` that shows each binding's action **by name** ("→ Jump", not "→ 9876543210987654321") and
  lets the author pick one. This single custom widget is what makes the binding table usable; it is
  still no press-a-key capture UX and no global registry (the combo is scoped to the document's own
  `actions`).
- **MCP reachability is a small override, not automatic.** To be inspectable/settable over the
  existing `editor.*` tools the panel overrides **`GetInspectables()`/`OnInspectableChanged()`**
  exposing its reflected document (as `LevelEditorPanel` does — a panel that adds no override is not
  MCP-reachable). No new MCP tool or schema: the generic reflected-inspector-is-the-API path consumes
  it, so the cost is the one override.

### 2. Live recook + preview

- Editing debounces (the established ~300 ms) and calls `EditorHost::RequestCook` on the
  `*.inputmap.json`; the cooked context hot-reloads behind its `AssetHandle` via `MountMemory`, so a
  running Play session in the editor picks up the new bindings immediately.
- **Preview** is a read-only resolved-state readout: the panel shows each declared action and its
  live resolved value/phase for the editor's own input. It drives Plan 00's `ResolveActions` over the
  document's `ResolvedContext` and the editor host's always-fed `Veng::Input` snapshot — read through
  the **public `RawInput` adapter Plan 01 exposes**, not `UI::Query` (which is UI-logic-only, not the
  raw device snapshot the resolver needs) — each frame, threading the prior frame's `ActionState` as
  `previous`. This makes "did my WASD binding take" observable without launching the game — the
  input-editor analogue of the material preview sphere. Known limitation: a keystroke captured by an
  active text widget in the panel won't reach the snapshot. It reuses the pure resolver directly; no
  new mechanism.

### 3. Save-back

The reflection-driven `*.inputmap.json` write preserves unknown keys and stable ordering, matching
the texture/material round-trip (each single-asset editor hand-writes its known-key patch/preserve
pass; this panel does the same — it is not a free generic writer). **No per-document undo:** the
single-asset editors carry no `CommandStack`, so neither does this panel; adding undo to that whole
family is a roadmap item (Plan 04), not this plan.

## Notes & constraints

- **Basic by design.** No press-a-key-to-bind capture, no action-dropdown driven by a global
  registry (there is none), no drag-reorder. Those are the rich-editor investment deferred until a
  data consumer of actions exists (README, *What remains future*). The bar here is "a developer can
  view and edit the binding table in the editor without hand-writing JSON," which the reflected
  inspector clears essentially for free.
- **Editor-only.** `libveng_editor`/the `veng-editor` exe; `libveng` and a game module gain nothing.
  The panel links no new dependency.
- **The live-preview resolver call is editor input**, read through the editor host's `Veng::Input`
  snapshot (via Plan 01's public `RawInput` adapter), not a seat and not `UI::Query` — it visualizes
  the *context*, independent of any running seat.

## Files (sketch)

- `editor/src/panels/InputMappingEditorPanel.h` + `.cpp` — the panel (beside the other per-asset
  panels), deriving from `EditorPanel`; registers the `ActionId` field widget and `GetInspectables()`.
- `EditorHost`'s `RegisterEditors` — register the panel for `AssetType::InputMap`.
- Reflection metadata on `Binding`/`InputAction` (small edits in `Veng/Input/Actions.h`) for the
  labels/tooltips/categories the panel draws free.

## Verification

- **`veng-editor --project examples/hello-triangle/project.veng`**, open `gameplay.inputmap.json`:
  the actions + bindings draw editable, enums as combos, bindings show their action name; editing a
  binding and saving round-trips the JSON (unknown keys preserved); the live preview readout reflects
  the editor's WASD/Space.
- **MCP smoke:** with the panel's `GetInspectables()` override in place, its reflected document is
  inspectable/settable over the existing `editor.*` tools — **no new MCP tool or schema** (the
  reflected-inspector-is-the-API property). An MCP client sees raw numeric `ActionId`s (the panel's
  name combo is UI-only), matching every other asset's MCP surface.
- The SDK conformance path still builds the editor; `sdk_conformance_*` unaffected.
- Clean build, full `ctest` green.

## Dependencies

Plan 02 (the asset + loader the panel edits). Last feature plan before the closer.
