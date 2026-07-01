# Plan 04b — editor host tools + wiring the `veng-editor` exe

**Goal:** wire the `veng-editor` exe as an MCP host (behind `--mcp`) and add the editor's
**non-reflection** tools: open an asset editor, inspect the project's assets, cook an asset on demand,
and screenshot a panel. Together with Plan 04a's generic property/command tools, this is the editor's
full agent surface. `libveng_mcp` stays editor-free. Depends on Plan 04a.

## The starting point

- `veng-editor` is a single project-agnostic exe (built in `editor/`, links `libveng`,
  `libveng_editor`, `libveng_cook`), launched with `--project <project.veng>`
  ([editor/CLAUDE.md](../../editor/CLAUDE.md)). `EditorHost` is an `Application` subclass owning the
  panel set, the dockspace, the `EditorRegistry`, and the live document scene(s).
- `PanelHost::OpenAssetEditor(AssetType, AssetId)` already exists and is deferred-safe — it adopts the
  panel into the set at the next safe frame point (`editor/src/EditorHost.cpp:555`). Opening a panel is
  a one-line passthrough; showing/hiding one is flipping `PanelSlot::Open` (the Window menu already
  does this).
- `EditorHost` builds an **`AssetSourceIndex`** from the union of the project's pack manifests
  (`AssetSourceIndex::ParsePacks`) — the id → name/type/source map the inspector's chips and pickers
  already search.
- `EditorHost::RequestCook(CookRequest, callback)` cooks a single source off the render thread via
  `TaskSystem`, mounts the in-memory archive, and hot-reloads behind the stable handle.
- Scene panels own registered `Offscreen` viewports the engine renders (`SceneViewportPanel`), so a
  panel screenshot reuses Plan 02's `Download` → PNG path against the panel's viewport.

## What lands

### 1. `veng-editor` links `veng::mcp` and drives the server (behind a flag)

- The `veng-editor` **exe** links `veng::mcp` (not `libveng_editor` — the framework lib stays
  MCP-free, MCP stays optional even for the editor) and gains `--mcp[=port]` (default off; `--mcp`
  picks a documented/ephemeral port) and `--mcp-write` (enables `AllowMutations`, default off).
- On startup, when `--mcp` is set, the exe constructs an `McpServer` and fills the `McpHost` from the
  `EditorHost`: `Types`/`Assets` from the host; `CurrentWorld` returns the **focused** document
  editor's `Scene*` (or null when no document is open); `Viewport`/`ViewportNames` expose the open
  scene panels' viewports by a stable name; and `ApplyMutation` is the Plan 04a command-routing hook.
- `EditorHost` calls `server->Pump()` once per frame at a scene-safe point (before the panels iterate
  their scenes — the same window the editor's own after-the-walk edits land in).

### 2. Editor host tools (`RegisterEditorHostTools`, editor-side)

Registered beside Plan 04a's reflection tools, in the same `editor/src/EditorMcp.cpp`:

- **`editor.open_asset`** — arg `{ asset: <AssetId> }` → `PanelHost::OpenAssetEditor` (resolving the
  type from the `AssetSourceIndex`), returns the opened panel's identity. The "open the editor for
  this asset" verb.
- **`editor.set_panel_visible`** — arg `{ panel, visible }` → flips the panel's `Open` at the pump
  point (the Window-menu toggle, programmatic). Close reachable for document panels the same way the
  host already erases closed documents.
- **`editor.list_assets`** — arg `{ type?: <AssetType> }` → the `AssetSourceIndex` entries (id, name,
  type, source path), optionally filtered. Project-wide asset inspection an agent can browse.
- **`editor.screenshot_panel`** — arg `{ panel }` → the Plan 02 capture path against the named panel's
  `Offscreen` viewport. Lets an agent see a specific editor panel, not just the game view.
- **`editor.request_cook`** — arg `{ asset: <AssetId> }` → kicks `EditorHost::RequestCook` and returns
  `{ status: "started" }` immediately; a companion **`editor.cook_status`** (or the tool holding its
  request until the async callback lands — the implementing agent picks the simpler shape and
  documents it) reports completion. The editor-only cook-on-demand path, distinct from the runtime
  `world.load_prefab` which never cooks.

### 3. The `McpHost` closures are document-aware

`CurrentWorld`/`Viewport` track the **focused** document editor, so the world + render tools follow
whatever the editor is showing — open a different level and the same tools now read it. A
closed-document editor returns null / no-viewports, and the tools degrade gracefully (Plan 01's
null-world contract).

## Notes & constraints

- **`libveng_mcp` gains no editor dependency** — the direction is `editor → veng::mcp` throughout.
- **Dock-layout choreography is out of scope** (a documented non-goal for this planset): moving/
  splitting/retabbing panels programmatically is `DockBuilder` state manipulation + `.ini`
  reconciliation, fiddly and low agent value; floating OS windows are a standing design non-goal
  (multi-viewport is disabled). The window verbs here are list/open/show-hide/focus only.
- The exe's link to `veng::mcp` can be unconditional (MCP is now an SDK lib) with `--mcp` gating
  runtime activation, or fully conditional — the agent picks the smaller diff; either way the server
  is never constructed without `--mcp`, so the editor's existing smokes are unaffected when it is
  absent.

## Files (sketch)

- `editor/CMakeLists.txt` — the `veng-editor` exe links `veng::mcp`.
- `editor/src/EditorMcp.{h,cpp}` — `RegisterEditorHostTools` (+ Plan 04a's `RegisterEditorReflectionTools`
  and the `ApplyMutation` routing) and the editor-host `McpHost` construction.
- `editor/src/EditorHost.{h,cpp}` (or the exe `main`) — construct/own/pump the server behind `--mcp`;
  a small panel-list getter and a name→panel-viewport resolver for the seams.

## Verification

- A **GPU** editor smoke (labelled `gpu`, skips with no ICD): launch `veng-editor` against the sample
  project with `--mcp` on an ephemeral port (headless/scripted), and over loopback: `tools/list`
  (asserts `editor.*` + `world.*` + `render.*`), `editor.list_assets` returns the project's assets,
  `editor.open_asset` opens a document (then `editor.list_panels` shows it), `editor.screenshot_panel`
  returns a PNG. If a fully headless editor run is impractical, scope to the window-less parts and let
  Plan 05's conformance smoke cover the rest.
- Opening a different document changes what `world.list_entities`/`editor.inspect` report (the
  document-aware seam).
- `ctest` green; `include_hygiene` green; the editor's existing smokes unaffected when `--mcp` is
  absent.
