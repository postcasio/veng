# Plan 03 — the input-mapping editor panel

**Goal:** the almost-free editor. Register an `InputMappingEditorPanel` for `AssetType::InputMap`
that draws the reflected context — the binding table through the existing `DrawFieldWidget`,
plus action-name labels — with the same live recook → hot-reload → preview loop the texture and
material editors use. Deliberately **basic**: the reflected inspector, not a bespoke
press-a-key-to-bind capture UX. Depends on Plan 02.

## The starting point

- The editor registers a panel per asset type: `TextureEditorPanel` (`AssetType::Texture`),
  `MaterialEditorPanel` (`AssetType::Material`), the prefab/level editors. An `AssetEditorPanel` base
  gives the document lifecycle, the per-document undo/redo `CommandStack`, and the save/dirty
  handling; a panel gets its editable data drawn for free by walking reflected objects through
  **`DrawFieldWidget`** (editor/CLAUDE.md).
- `ProjectSettings` proves a whole reflected data model — including a `FieldClass::Array`
  (`vector<BuildConfiguration>`) — draws an add/remove/edit panel through `DrawFieldWidget` with **no
  bespoke widget code**. An `InputMappingContext` (`vector<InputAction>` + `vector<Binding>`) is the
  same shape.
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
  `axis` are `VE_ENUM` combos; `id`/`control`/`scale`/`invert` are scalar widgets. This is the whole
  editor with zero custom widgets — the "almost free" the panel is here to cash in.
- **Reflection metadata sharpens it cheaply.** `FieldDescriptor` `DisplayName`/`Tooltip`/`Category`
  on the `Binding`/`InputAction` fields (e.g. label `Control` "Key/Button code", group Source vs.
  Mapping) — authored on the structs in Plan 00/02, drawn free here. No panel code.
- **Action-name labels.** The one small non-free touch: when drawing a `Binding.action` (an
  `ActionId`), show the matching `InputAction.Name` from the same document beside the raw id, so a
  binding reads "→ Jump" not "→ 9876543210987654321". A tiny lookup in the panel over the document's
  `actions`; still no capture widget, no registry.

### 2. Live recook + preview

- Editing debounces (the established ~300 ms) and calls `EditorHost::RequestCook` on the
  `*.inputmap.json`; the cooked context hot-reloads behind its `AssetHandle` via `MountMemory`, so a
  running Play session in the editor picks up the new bindings immediately.
- **Preview** is a read-only resolved-state readout: the panel shows each declared action and its
  live resolved value/phase for the editor's own input (drive the Plan 00 `ResolveActions` over the
  document's `ResolvedContext` and the editor's raw input each frame, display the `ActionState`). This
  makes "did my WASD binding take" observable without launching the game — the input-editor analogue
  of the material preview sphere. It reuses the pure resolver directly; no new mechanism.

### 3. Save-back

The reflection-driven `*.inputmap.json` write preserves unknown keys and stable ordering, matching
the texture/material round-trip. Edits route through the panel's `AssetEditorPanel` `CommandStack` so
they are undoable, like every other asset editor.

## Notes & constraints

- **Basic by design.** No press-a-key-to-bind capture, no action-dropdown driven by a global
  registry (there is none), no drag-reorder. Those are the rich-editor investment deferred until a
  data consumer of actions exists (README, *What remains future*). The bar here is "a developer can
  view and edit the binding table in the editor without hand-writing JSON," which the reflected
  inspector clears essentially for free.
- **Editor-only.** `libveng_editor`/the `veng-editor` exe; `libveng` and a game module gain nothing.
  The panel links no new dependency.
- **The live-preview resolver call is editor input**, read through the editor's own `Veng::Input`
  (`UI::Query`/the router), not a seat — it visualizes the *context*, independent of any running
  seat.

## Files (sketch)

- `editor/src/InputMappingEditorPanel.h` + `.cpp` — the panel.
- `editor/src/EditorRegistry` wiring — register it for `AssetType::InputMap`.
- Reflection metadata on `Binding`/`InputAction` (small edits in `Veng/Input/Actions.h`) for the
  labels/tooltips/categories the panel draws free.

## Verification

- **`veng-editor --project examples/hello-triangle/project.veng`**, open `gameplay.inputmap.json`:
  the actions + bindings draw editable, enums as combos, bindings show their action name; editing a
  binding and saving round-trips the JSON (unknown keys preserved); the live preview readout reflects
  the editor's WASD/Space.
- **MCP smoke** (the editor exposes panels through `GetInspectables()`): the input-map panel's
  reflected document is inspectable/settable over the existing `editor.*` tools with **zero MCP
  change** — the reflected-inspector-is-the-API property, confirming the panel added no parallel
  surface.
- The SDK conformance path still builds the editor; `sdk_conformance_*` unaffected.
- Clean build, full `ctest` green.

## Dependencies

Plan 02 (the asset + loader the panel edits). Last feature plan before the closer.
