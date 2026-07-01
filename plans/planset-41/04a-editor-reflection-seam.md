# Plan 04a — the editor panel reflection seam + generic editor tools

**Goal:** let an agent read and edit **what an asset editor is editing** — a level's render settings, a
material's params, a texture's settings — and drive the shared lifecycle commands (save, undo, redo),
**without** the MCP library mirroring any panel's API. The mechanism is a small generic seam on the
editor panel base that hands back the reflected object(s) a panel edits; the MCP tools consume it with
the same `ReflectToJson`/`JsonToFields` the inspector consumes through `DrawFieldWidget`. When the
editor changes, the reflection changes in one place and both the inspector and the MCP surface follow —
there is no second API surface to keep in sync. Depends on Plans 02 + 03.

## The starting point — the editor already edits through reflection

- The inspector's per-field primitive is generic: `DrawFieldWidget(void* fieldPtr, const
  FieldDescriptor&, const FieldWidgetContext&)` (`editor/src/FieldWidget.cpp:588`) — `void* +
  FieldDescriptor`, the exact shape Plan 01/03's `FieldsToJson`/`JsonToFields` are on the read/write
  side. Panels edit their reflected members by handing this a pointer; they do not hand-roll an
  editing API.
- `AssetEditorPanel` (`editor/src/AssetEditorPanel.h`) already exposes the non-field verbs as
  virtuals: `Save()`, `HasUnsavedChanges()`, `GetCommandStack()` (the undo/redo stack), and
  `IsDocumentFocused()`. The host dispatches File/Edit-menu actions and shortcuts to the focused
  document through them. These are the lifecycle command seam — already written.
- The panels' edit targets are reflected objects the panels hold: the **level editor** draws
  `LevelRenderSettings` + `GameModeConfig` through `DrawFieldWidget` (plus the world scene, already
  reachable via `McpHost::CurrentWorld`); the **material editor** holds the material's params
  (reflected `MaterialField`s); the **material-instance editor** edits a sparse override set over the
  parent's `GetFields()`; the **texture editor** edits its `.tex.json` settings.
- `Scene::AddComponent(Entity, TypeId)` and the `CommandStack` (`editor/src/CommandStack.h`,
  `AddComponentCommand`) are what the editor's own edits go through — so an agent edit routed through
  them is undoable and marks the document dirty identically.

## What lands

### 1. The panel reflection seam

Add to the panel base (`EditorPanel` or `AssetEditorPanel` — whichever the panels that need it derive
from; the asset editors derive `AssetEditorPanel`, the texture/material editors are their own base, so
the seam likely lives on `EditorPanel` so all can implement it):

```cpp
/// @brief One reflected object this panel edits, named for addressing.
struct Inspectable { Veng::string Name; Veng::TypeId Type; void* Data; };

/// @brief The reflected models this panel exposes to inspection/editing. Default empty.
virtual Veng::vector<Inspectable> GetInspectables() { return {}; }

/// @brief Called after an external write into an inspectable, so the panel runs its
/// existing apply path (recook, mark dirty, re-resolve). Default no-op.
virtual void OnInspectableChanged(Veng::string_view name) {}
```

Panels implement `GetInspectables()` by returning the 1–3 reflected objects they already hold — a
thin accessor beside where they already call `DrawFieldWidget`:

- **Level editor** → `{ "renderSettings": LevelRenderSettings, "gameMode": GameModeConfig }` (the
  world scene stays served by the world tools).
- **Material editor** → `{ "params": <the reflected param object> }` (rich node-graph editing is
  **not** here — see the deferred note).
- **Material-instance editor** → `{ "overrides": … }` over the parent schema.
- **Texture editor** → `{ "settings": <the .tex.json settings> }`.

`OnInspectableChanged` maps to each panel's existing reaction — the material/texture editors' debounced
recook, the level editor's dirty flag + live `ApplyLevelRenderSettings` preview.

### 2. Generic editor property/command tools (`RegisterEditorReflectionTools`)

Registered from the editor side (see Plan 04b's wiring), fully generic over the seam — **no per-panel
code in the MCP layer**:

- **`editor.list_panels`** — the host's panels: title, open/closed, focused, kind, and the names of
  their inspectables. The map of what's editable right now.
- **`editor.inspect`** — arg `{ panel, inspectable? }` → `FieldsToJson` of the named inspectable (or
  all of them). "Read the level's render settings", "read the material's params" — for any panel,
  present or future.
- **`editor.set_field`** — arg `{ panel, inspectable, values: { <path>: <value> } }` → `JsonToFields`
  into the object, then `OnInspectableChanged(inspectable)` so the panel recooks / marks dirty exactly
  as a UI edit would. Add a field to `LevelRenderSettings` and it is settable over MCP with zero MCP
  change.
- **`editor.save`** / **`editor.undo`** / **`editor.redo`** — the `Save()` / `GetCommandStack()`
  virtuals; error results where the focused document offers none (the base returns "no save action").

### 3. Editor mutations route through the command stack

The editor host fills Plan 03's `McpHost::ApplyMutation` hook so the **world** mutation tools
(`entity.add_component`, `set_field`, `spawn`, `destroy`) against a document scene push the
corresponding editor commands (`AddComponentCommand`, …) onto the focused document's `CommandStack`
instead of touching the scene raw. So an agent's "add a Light to entity 3" is **undoable** and marks
the document dirty, identical to the human Add-Component path. A game host leaves the hook null (raw
scene). This is the whole of the editor-vs-game mutation difference — the tools are unchanged.

## The end-to-end workflow this enables

`editor.list_panels` (find the open level editor) → `world.list_entities` (the document scene, via the
focused-document `CurrentWorld`) → `entity.add_component { id, component: "Veng::Light" }` (routed
through the command stack, undoable) → `editor.inspect { panel, "renderSettings" }` /
`editor.set_field` to tune the level → `editor.save`. All generic; none of it names a specific panel
in the MCP library.

## Notes & constraints

- **Direction is `editor → veng::mcp`.** The seam methods live on the editor panel base and the tools
  register from the editor side; `libveng_mcp` gains no editor dependency. This mirrors `veng::graph`'s
  editor-free posture.
- **Inspectable pointers must be stable for the frame**, and writes land at the pump point — the same
  guarantees a `DrawFieldWidget` edit relies on. A panel that rebuilds its model each frame returns the
  current pointer each `GetInspectables()`.
- **No method/function reflection.** The property surface is data reflection (already have it); the
  action surface is the small bounded lifecycle-verb set (already virtuals). A general reflected-command
  / scripting layer is parked as future (see below) — building it to serve ~5 editor verbs would be a
  whole subsystem for no proportionate gain.

## Files (sketch)

- `editor/include/VengEditor/EditorPanel.h` — the `Inspectable` struct + the two virtuals.
- `editor/src/AssetEditorPanel.{h,cpp}` + the 4 asset-editor panels — `GetInspectables()` /
  `OnInspectableChanged()` overrides (thin, beside their `DrawFieldWidget` sites).
- `editor/src/EditorMcp.{h,cpp}` (new; the registration side, shared with Plan 04b) —
  `RegisterEditorReflectionTools` + the command-routing `ApplyMutation` implementation.

## Verification

- A headless/editor test (labelled `gpu` where a document scene needs a device): open a level editor
  document, and over loopback `editor.list_panels` (asserts the level editor + its inspectable names),
  `editor.inspect` the render settings (round-trips a known value), `editor.set_field` one and confirm
  the document is now dirty (`HasUnsavedChanges`), `entity.add_component` a Light through the routed
  path and confirm `editor.undo` removes it (command-stack routing works).
- `include_hygiene` green; `libveng_mcp` still links no editor target.

## Deferred (noted in Plan 06's roadmap pass)

- **Rich node-graph editing** (add/connect nodes in the material editor) — exposed later through the
  `NodeGraph` serialize + mutation vocabulary (`veng::graph`), **not** a hand-mirrored node API. The
  same "consume the existing serializer" principle, one seam deeper.
- **General reflected commands / a scripting surface** — if a broad need emerges (data-driven command
  palette, gameplay-system actions), method reflection earns its keep and the MCP tools ride it for
  free. Not built for editor lifecycle verbs alone.
