# libveng_editor — the editor framework

The editor is a separate executable, not part of the runtime. It is built on the engine
([engine/CLAUDE.md](../engine/CLAUDE.md)) — `Veng::UI`, `SceneRenderer`, `AssetManager`, and the
reflection layer — and on the cook pipeline ([cooker/CLAUDE.md](../cooker/CLAUDE.md)) for its
cook-on-demand loop. Project-wide conventions live in the [root CLAUDE.md](../CLAUDE.md); the
node-graph + material-codegen library both the editor and the cooker link is
[graph/CLAUDE.md](../graph/CLAUDE.md).

`libveng_editor` is the editor **framework** library; the editor is a **single, project-agnostic
`veng-editor` exe** (built in `editor/`, links `libveng`, `libveng_editor`, and `libveng_cook`) —
**not** a per-game binary.

## The exe: SDK surface, flags, project launch

Both `libveng_editor` and `veng-editor` are an **installed SDK surface**, gated behind the
`VENG_INSTALL_SDK` option (default `${PROJECT_IS_TOP_LEVEL}`) that gates the editor build/install.
`find_package(veng)` exports `veng-editor` as an **imported executable** in `vengTargets`;
`veng-config` recreates the unqualified `veng-editor` name and the `veng_editor::veng_editor`
library alias with `NOT TARGET`-guarded aliases, so `veng_add_editor` resolves the imported exe
and a downstream `EDITOR_MODULE` links `veng_editor::veng_editor` in every consumption mode. The
installed `veng-editor` carries an `INSTALL_RPATH` and requires the host's Vulkan SDK / Slang at
runtime. **`veng-editor --version`** prints `veng-editor <version>` and exits before opening a
window or creating a device — the SDK identity probe the conformance tests run.

**`veng-editor --connect=<port|host:port>`** is the client mirror of `--mcp`: where `--mcp[=port]`
(add `--mcp-write` for mutations) runs the editor's own MCP *server*, `--connect` makes the exe a
one-shot *client* of an already-running MCP server — it drives one tool call (or `--list`) and
`return`s **before** any window or device is created, exactly as `--version` does, calling the
shared `Mcp::RunClientCli` with `"veng-editor"` as its error label (the grammar + exit-code map
live in [mcp/CLAUDE.md](../mcp/CLAUDE.md)). Both the `--mcp` server and the `--connect` client —
the whole MCP surface of the exe, plus `EditorMcp.cpp` and the `veng::mcp` link — are gated behind
the **`VENG_EDITOR_WITH_MCP`** cache option (default **on**); building it off yields an MCP-free
editor with no `veng::mcp` link and neither flag. (It is distinct from the unrelated
`VENG_EDITOR_MCP` source-path CACHE INTERNAL string.)

The editor is launched with `--project <project.veng>`; it reads the module(s) the project names
(`ProjectSettings::Module` / `EditorModule`, from the `"module"` / `"editorModule"` keys) and
`dlopen`s them from the project's **build-output dir** the same way the launcher does — but
passing a non-null `EditorRegistry*` in `VengModuleHost::Editor`. A game ships only its library,
referenced from the project file. The build-output dir (cooked packs + module libraries) is
**discovered from the project**: the build records it in a gitignored `.veng/build.json` sidecar
beside the source `project.veng`, so launching with only a project resolves it — no CMake in the
launch loop (`--build-dir` stays an override). `veng_add_editor` builds no exe: it writes that
sidecar, places an optional editor-extension module beside the launcher, and registers a
per-project `<name>-editor` **run target** that launches `veng-editor` with the project's source
`project.veng`. **Same-tree only** — a module must be built from the editor's own source tree
(`VengModuleAbiVersion` rejects a mismatch at load).

**The editor opens the project, not a manifest.** `EditorHostInfo::ProjectPath` (the source
`project.veng`, passed by `--project`) is the editor's entrypoint: `EditorHost::Create` reads it
through `LoadProjectSettings` (the host-owned `ProjectSettings` — its `Configurations`,
`ActiveConfiguration`, `Packs`, `StartupLevel`, and the `Module` / `EditorModule` names it
`dlopen`s), then resolves each module as `lib<name>.<ext>` beside the build output. **The build
output dir is resolved once in `Create`** (`m_BuildDir`): an explicit `EditorHostInfo::BuildDir`
(`--build-dir`) override, else discovery from the `.veng/build.json` sidecar beside the project
(`DiscoverProjectBuildDir`), else `ExecutableDirectory()` (the relocatable ship layout). The
editor mounts each cooked pack from `m_BuildDir` (under the source manifest's stem) and builds its
`AssetSourceIndex` from the **union** of the project's pack manifests
(`AssetSourceIndex::ParsePacks`). The editor's own icon pack stays beside the editor exe
(`ExecutableDirectory()`), distinct from the project's build dir. The runtime `.vengproj` is a
game-launch artifact the editor does not consume. Cook-on-demand passes **every** project pack as
a reference (`CookRequest::ReferenceManifests`), so an edited asset resolves cross-asset ids
across the whole project's one AssetId namespace, not just its own pack.

## Host, panels, and viewports

- **`EditorHost`** is an `Application` subclass living in `libveng_editor`. It builds a top-level
  single-window `DockSpace` (`ImGuiConfigFlags_DockingEnable`; multi-viewport OS windows are not
  built — they conflict with the single-offscreen-composite model), owns the panel set
  (open/close state, Window menu, dock layout), and loads the game module with
  `Editor = &m_EditorRegistry`. **It runs the engine render tail**, not a hand-rolled present
  path: a render-owning panel holds its own `Viewport` and registers it into the base
  `Application` drive-list, so the engine renders every panel viewport, then `ImGuiLayer::Render`
  + the managed gather/composite bracket `OnRender` in the base. The editor registers **no**
  `Presented` viewport, so the composite runs with **zero placements** (a cleared assembly target,
  ImGui only) — the editor's content is the ImGui dockspace, and each panel's scene is an ImGui
  texture over an `Offscreen` viewport.
- **`EditorPanel`** is the panel base class: a `GetTitle()` / `OnUI()` virtual interface plus a
  `Draw(bool* open)` seam (default wraps `OnUI` in one `UI::Window`). It carries **no render
  seam** — a render-owning panel holds a registered `Veng::Renderer::Viewport` (`Offscreen`) and
  the engine drive-list renders it each frame; the panel pushes the viewport's `ViewState` and its
  region (from the ImGui content rect) in `OnUI`, and samples the ready output as a `UI::Image`.
  The host drives each open panel and owns its visibility. Top-level host panels: asset browser,
  console/log, and the per-asset editors below.
- **An editor writes the user's files only on an explicit save — this is the editor-wide rule, and
  it has no exceptions.** No auto-save, no debounced recook, and **nothing writes on a timer**: edits
  accumulate in the panel's in-memory document, and `Save()` is what performs the
  preserve-unknown-keys merge write, then the recook, then the hot reload — **in that order**, so a
  cook that fails leaves the saved source on disk and reports in-panel rather than reverting the
  edit. A write that fails clears no dirty flag and cooks nothing, so the editor still holds the
  edits it could not persist.
  **`AssetEditorPanel` is the contract, and every asset editor derives from it** — that
  `dynamic_cast` is what `EditorHost::FocusedAssetEditor` finds, and the only route to File▸Save and
  Ctrl/Cmd+S. It carries the whole thing: `HasUnsavedChanges()` drives the window's unsaved marker
  and the Save action's enabled state, `EditorHost` dispatches the File-menu item and Ctrl/Cmd+S to
  the focused document, and `AssetEditorPanel::Draw` **takes back a close** on a dirty document and
  raises a Save / Discard / Cancel prompt (the host destroys a panel whose open flag clears, so that
  is the only place to ask) — a subclass writes nothing for the prompt. A toolbar Save button wraps
  in `UI::Disabled(!dirty)`. A recook arriving while one is in flight is **queued**, not dropped —
  the in-flight cook read an older source, so a save landing behind it must re-cook once it lands.
  `VengEditor/AssetSaveModel.h` is the shared, UI-free form of both halves — `SaveAssetSource` (the
  ordered write → clear-dirty → cook sequence) and `CookGate` (`Request`/`Complete`, one in flight
  and at most one queued) — which is what makes the model testable in the device-free `editor_unit`
  band. It and `AssetEditorPanel` are public `VengEditor/` headers, so a game-defined asset type can
  ship a first-class save/preview editor (the `AssetEditorContext` carries the audio engine, the
  host task system, the status tracker, the asset's source path, and the recook `CookDriver` a game
  factory needs beyond the render context — a game panel's background task brackets itself with the
  status tracker's `Begin`/`End` so it shows in the status bar exactly as the cook does). **No panel carries a countdown that reaches a file**: a cook debounce that *follows* an
  explicit save would not be an auto-save, but none exists either — the sole per-frame countdown in
  a panel is the material editor's status toast.
- **`AssetEditorPanel` hosts a private, class-restricted dockspace.** An asset editor is a
  top-level panel whose window hosts a per-instance ImGui dockspace; its child panels are
  submitted as separate windows tagged with a per-instance `ImGuiWindowClass`, so only that
  editor's children dock into its area and cannot stray into the host dockspace (two open editors
  of the same asset stay isolated by a monotonic instance id). A subclass adds children with
  `AddChild` and arranges the initial split in `BuildDefaultLayout`; it overrides `Draw` to submit
  the document window + dockspace + the class-tagged children. A child that renders a scene owns
  its own registered `Offscreen` viewport, so there is no render forwarding. An editor that adds
  **no** children hosts no dockspace at all — nothing could dock into it, and an empty `DockSpace`
  would eat the window's content region — so its `OnUI` fills a plain single window and
  `BuildDefaultLayout` (defaulted to a no-op) is never reached.
- **`EditorRegistry`** is defined in `libveng_editor` and **forward-declared** in
  `engine/include/Veng/Module/Module.h` (so `libveng` stays clean). It holds the
  `AssetTypeId`→editor-factory map (double-click an asset opens its editor), `RegisterPanel` for
  game-contributed panels, and `RegisterFieldWidget(TypeId, FieldWidgetFn)` for custom inspector
  widgets. It is non-null in `VengModuleHost` only in the editor host.

## Scene editing: the prefab and level editors

- **The prefab editor is the scene-editing surface.** `PrefabEditorPanel` (registered for
  `AssetTypes::Prefab`) loads + `SpawnInto`s the prefab into a document-owned live `Scene` (adding
  a default directional light when the prefab carries none) and hosts three children over one
  shared `PrefabEditContext` (`Scene*` + `AssetManager*` + a multi-entity `Selection` + the
  `Active` entity + the `EntityPayload` drag tag + a `ResolveEntity` helper): `SceneViewportPanel`
  (owns a registered `Offscreen` `Viewport` the engine renders, samples its output into a
  `UI::Image`, feeds the viewport's region from the panel content rect and pushes an orbit-camera
  `ViewState` each frame, with the `DebugView` dropdown; `Viewport::ScreenToWorldRay` is the
  entity-picking seam), `PrefabExplorerPanel`, and `InspectorPanel`. The host opens the sample
  prefab as the initial document; double-clicking a prefab in the asset browser opens another.
- **`PrefabExplorerPanel` is a full scene-graph tree** over the intrusive `Hierarchy`
  ([engine/src/Scene/CLAUDE.md](../engine/src/Scene/CLAUDE.md)): roots are entities with a null
  parent, children walk `ForEachChild` in order. It drives the shared selection (click /
  Ctrl-click toggle), inline-renames (double-click), drag-reparents (`Scene::SetParent`) and
  reorders siblings (`Scene::MoveBefore`) — both with a cycle pre-check so the engine's fatal
  cycle assert is never reached — and creates / adds-child / duplicates / deletes entities, plus a
  name filter and row/empty-space context menus. Delete is reachable three ways: the toolbar trash
  button, the row context menu, and the `Delete` key over the selection. Every structural edit is
  **queued during the draw and applied after** the snapshot walk returns, so nothing mutates the
  scene mid-iteration (the `Scene` contract). Duplicating an entity round-trips its components
  into the copy but builds no derived mesh, so `DuplicateSubtree` calls `ResolveEntity` on each
  copy after its children recurse — the byte copy carries the recipe `Source` forward but never
  the built handle.
- **`PrefabSerialize` is the write inverse of the cooker's `PrefabImporter`, through the same
  shared walker.** Saving a document writes each entity's components via the **merge-write**
  `JsonWriteFields(existingComponentJson, componentPtr, typeInfo, registry, hooks)`
  ([engine/src/Reflection/CLAUDE.md](../engine/src/Reflection/CLAUDE.md)) — patching the
  reflected fields into the JSON already read from source, so unknown keys (comments-as-keys,
  hand-authored structure) survive untouched. Its `JsonFieldHooks::WriteReference` maps a live
  `Entity` back to a prefab-local index, the inverse of the importer's `ReadReference`; enums come
  out as enumerator names, matching what the cooker's `JsonReadFields`-based read requires.
- **The level editor is the game-wiring surface.** `LevelEditorPanel` (registered for
  `AssetTypes::Level`) **derives from** `PrefabEditorPanel`, so the viewport / explorer / inspector
  edit the level's **world prefab** with no scene-editing reimplemented, and adds two level-scoped
  children over the same dockspace: a **systems panel** listing the `SystemRegistry` catalog with
  a per-system enable toggle, phase labels, and drag-reorder over the active set — writing the
  level's ordered `SystemId` list — and a **settings panel** drawing the `GameModeConfig` and the
  post/pipeline `LevelRenderSettings` through the shared reflection inspector
  (`DrawFieldWidget`). The sky is **not** in that panel: it is the scene's author-opt-in `Sky`
  component (plus an optional `TimeOfDay`), added to a world entity through the ordinary inspector
  Add-Component surface and resolved by the renderer itself each frame — so the sky appears the
  moment the component is added. System *params* stay components, edited through the world surface
  like any other; the level editor adds **no new inspector machinery** — the catalog drives the
  systems panel and reflection draws the config. Config edits (systems / game-mode / render)
  accumulate in memory and preview live in the viewport (render settings push through
  `ApplyLevelRenderSettings`); they persist only on **Save**, which writes both the world
  `*.prefab.json` (the base scene save) and the `*.level.json` config — the config record binds
  through the same shared `JsonReadFields` (tolerant, `allowUnknownFields = true`) and merge-write
  `JsonWriteFields` the cooker's `LevelImporter` reads with, so the panel's config round-trip is
  the walker's write inverse rather than a hand-mirrored copy — then recooks the level off the
  render thread and hot-reloads behind the stable handle. The document's unsaved-changes marker
  and the Save action's enabled state fold the config dirtiness in alongside the command stack
  (`HasUnsavedChanges`). Play runs **exactly the level's ordered system set** through the base's
  play machinery (`GetPlaySystems`), distinct from a bare prefab document's "all registered" set.
- **The editor's Play seat is single, keyboard/mouse; multi-seat is a game-runtime concern.**
  Play ticks the play-clone `SceneSimulation` (`PrefabEditorPanel::TickPlaySimulation`) with a
  `SystemContext{ .Assets, .Input }` that leaves `Pointer` at its default (empty `PointerRouting`,
  `Owner == Entity::Null`), so `InputMappingSystem` resolves each authored `SeatInput` seat's
  device-and-keyboard arms but every seat reads **neutral pointer** — the editor's play scene
  renders through its own `Offscreen` `SceneViewportPanel` viewport, which is never a `Presented`
  managed viewport and so is neither gathered into the window nor pointer-associated with the
  `InputRouter`. Split-screen (`ReconfigureManagedViewports`, the managed viewport list, and the
  region-gated pointer) is an `Application`-level game-runtime capability the editor does not
  exercise: it registers no `Presented` viewport, drives no managed-viewport list, and previews a
  scene's single authored `Viewer` seat. A seat's `SeatInput` is edited through the ordinary
  reflection inspector like any other component.

## The reflection-driven inspector

- **The widget-drawing core lives in the engine** — `Veng::UI::DrawFields` / `DrawFieldWidget`
  (`Veng/UI/Inspector.h`, in `libveng`) is the reflection inspector every consumer shares (a game
  UI links only `libveng`, not the editor framework, for it). It draws a built-in widget per
  `FieldClass` (Scalar/Vector/Quaternion/String/Enum/Matrix/Struct/Variant/Array), honors
  `FieldDescriptor::ReadOnly`/`Hidden`/`Tooltip`/`Category`, recurses nested
  structs/arrays/variants as flattened indented rows, and gates each field on VisibleIf/EnabledIf
  (`Veng/Reflection/FieldGate.h`).
- **The editor's `DrawFieldWidget` is a thin hook provider** (`editor/src/FieldWidget.{h,cpp}`,
  taking a `FieldWidgetContext { AssetManager&, const AssetSourceIndex&, const EditorRegistry& }`):
  it builds `Veng::UI::InspectorHooks` supplying the editor-only pieces the engine core can't
  resolve — the `AssetHandle` asset chip, the `Reference` Entity drop target, and the
  `EditorRegistry`'s per-`TypeId` custom widgets (the registered `LightType` combo) — and
  delegates to the engine walk. A bare game passes no hooks, so AssetHandle/Reference fields draw
  the engine's read-only fallbacks. The entity inspector and the node-property inspector both call
  `DrawFieldWidget`, so the two share identical widget behavior.
- **`DrawFieldValue` is the same widget without the row.** `DrawFieldWidget` emits a property-table
  row (label in column 0, value in column 1); a caller laying out its own grid needs only the value,
  where the grid's column header already names it and a row advance would break the layout. The data
  table's cells are the motivating case. Composite classes (Struct / Variant / Array) draw nothing
  and return false — they expand into several rows and have no single-value rendering.
- **`InspectorPanel`** edits `PrefabEditContext::Active`: an editable name header, a searchable
  **Add Component** picker (every registered `FieldClass::Struct` type not already present, minus
  the hierarchy-owned `Hierarchy`), and per-component remove / reset-to-default — remove offered
  both as a right-aligned button overlaid on the component header (the header sets
  `SetNextItemAllowOverlap` so the button takes the click) and in the header context menu, queued
  and applied after the `Scene::ForEachComponent` walk (the `Hierarchy` component is
  hierarchy-panel-owned and offers neither). Each component's fields render in a two-column
  `UI::PropertyTable` via the shared `DrawFieldWidget`. The **`Variant` widget** is a combo over
  the alternatives' display names (plus "(none)") that `SetActive`s the chosen alternative on
  change and recurses the active member's fields as indented rows — so a `MeshRenderer`'s `Source`
  shape variant gives primitive-kind selection and per-shape parameter editing for free.
  `DrawFieldWidget` returns a `bool changed` (accumulated through its nested-struct/variant
  recursion); `DrawComponent` ORs it across the component's fields and, when true, calls
  `PrefabEditContext::ResolveEntity` so an edit to a recipe source (its shape/parameters) rebuilds
  the derived mesh.
- **A mesh source re-resolves like any asset field.** A `MeshRenderer` carrying an inline recipe
  `Source` ([engine/src/Asset/CLAUDE.md](../engine/src/Asset/CLAUDE.md)) builds its mesh during
  `Prefab::SpawnInto`'s populate pass; there is no per-frame scan. An inspector edit to the source
  repoints the mesh exactly as repointing a cooked `AssetHandle` field would. Three triggers
  funnel through `PrefabEditContext::ResolveEntity`, which rebuilds the entity's `Mesh` from a
  non-empty `Source` via `BuildPrimitiveMesh`: Add Component, an inspector field edit (both gated
  on the `DrawFieldWidget` changed-bool), and the **duplicate** path — a `DuplicateSubtree` byte
  copy has no inspector edit to hook, so it rebuilds the derived mesh from the copied source
  explicitly.
- **The asset chip is the shared asset stand-in.** `DrawAssetChip(AssetChipInfo, AssetSourceIndex)`
  (`editor/src/AssetChip.{h,cpp}`) renders a bordered icon-plus-text box for an `AssetId`,
  optionally a drag source (emitting an `AssetDragPayload`) and/or a drop target. It is the asset
  browser's drag stand-in and the inspector's `AssetHandle` widget — which shows the type icon
  plus the asset's name / type / id, accepts a same-type asset dropped from the browser, and
  doubles as a selector: clicking it opens a searchable popup over the `AssetSourceIndex` entries
  of the field's `AssetTypeId` (with a "(none)" clear). The `AssetTypeName` / `AssetTypeGlyph` /
  `AssetTypeColor` type-metadata helpers live beside it, shared by the browser's badges; the
  first two read the host-owned `AssetTypeRegistry` the `AssetSourceIndex` was parsed against
  (reached through `AssetSourceIndex::GetAssetTypes()`), so a game-registered type displays under
  its registered name, while the colour table covers only the engine's own types and anything
  else takes a neutral grey.

## Cook-on-demand and the single-asset editors

- **Cook-on-demand keeps the importer boundary.** `libveng_cook` is linked **only into the editor
  exe** — never `libveng_editor`, never `libgame` — so the editor framework library stays
  importer-free. The exe injects a `CookBackend` implementation;
  `EditorHost::RequestCook(CookRequest, callback)` cooks a single source off the render thread via
  `TaskSystem` (`CookSession` → `Task<vector<u8>>`), then mounts the resulting in-memory archive
  via `AssetManager::MountMemory` and hot-reloads behind the stable `AssetHandle`.
- **The texture editor is the template.** `TextureEditorPanel` previews via a render target
  (`CreateTexture` → `ImGui::Image`), edits `.tex.json` settings (sRGB + sampler filter/wrap),
  and on save round-trips the JSON — patching known keys, preserving unknown ones — then recooks to
  refresh the preview. It carries a **compression-role combo** over the same round-trip
  (writing/clearing the `"role"` key) and shows the **resolved format read-only** for the active
  configuration ("→ ASTC4x4Srgb for 'macos'"), so the artist picks intent and reads the platform's
  codec without choosing one.
- **The material-instance inspector is the cheap-override authoring surface.**
  `MaterialInstanceEditorPanel` (registered for `AssetTypes::MaterialInstance`) edits a
  `*.vmatinst.json`: a **parent picker** (an `AssetChip` drop target of type `Material`) plus a
  **per-field override toggle** over the parent's exposed `GetFields()` — toggling a field on adds
  it to the sparse override set, off reverts it to the parent default — so the authored surface
  and the cook-validated surface are the same set by construction. Each param slot draws a
  `UI::Drag` over its component count and each texture slot an `AssetChip`; an un-toggled slot
  shows the parent default (read from the parent's `GetDefaultBlock()`) disabled. It previews
  through the **same** `MaterialPreview` path the material editor uses (the instance over its
  parent on a turntable sphere). Changing the parent reloads the schema and drops the prior
  overrides. Save merge-writes the `*.vmatinst.json`, then recooks and hot-reloads behind the
  stable handle.
- **The input-map editor is near-free.** `InputMappingEditorPanel` (registered for
  `AssetTypes::InputMap`) draws a `.inputmap.json`'s reflected document — its
  `vector<InputAction>` actions and its `vector<Binding>` bindings — through the shared reflection
  inspector (`DrawFields` over the same `FieldClass::Array` path the project-settings panel uses),
  so the binding table is add/remove/edit-able with **no** bespoke widget code. The one custom
  widget is an `ActionId` name combo scoped to the document's own declared actions (a `u64` leaf
  has no default scalar widget), so a binding picks its action by name, not a raw id. Save
  merge-writes the document, then recooks and hot-reloads behind the stable handle. It exposes a
  `GetInspectables()` override for the editor MCP — an external write through it marks the document
  dirty exactly as a UI edit does, and reaches disk only through `editor.save` — and draws a **read-only resolved-state preview** — the actions the current bindings
  resolve to over the editor's own input each frame — so a binding's effect is observable without
  launching the game. It is deliberately **basic by design**: no press-a-key-to-bind capture, no
  drag-reorder, no undo (the single-asset editors have none), matching the texture/material editor
  idiom.
- **The UI document editor authors markup, and the markup is its document.** `UIDocumentEditorPanel`
  (`AssetTypes::UIDocument`) edits a `*.vui.xml`: a WYSIWYG canvas — an `Offscreen` `Viewport`
  rendering the live `Gui::Document` instantiated from the cooked recipe, sampled into a `UI::Image`
  — plus an element-tree outline and a read-only resolved-style inspector, whose selected `<Image>`
  gets an editable texture asset-chip over its `src`. Every authoring action (**Add Image**, the
  chip) is a text rewrite of the in-memory markup held in `panels/UIDocumentSource.{h,cpp}` — the
  UI-free document model, which is what makes the mutations and the save contract testable in the
  device-free `editor_unit` band. Save writes the `*.vui.xml` and recooks; **Revert** reloads the
  file, which is also how an edit made in an external editor reaches the canvas. Because the cook
  reads the *file*, the canvas shows the last saved markup and an unsaved edit appears on save —
  the panel says so inline rather than looking dead. **This is a behavioral change for anything
  driving the editor externally**: an edit action used to write through to disk on its own, and now
  reaches disk only through an explicit save (`editor.save`, File▸Save, Ctrl/Cmd+S), exactly as an
  external write through `InputMappingEditorPanel`'s inspectables does. An agent workflow that
  relied on the write-through breaks by design.
- **The table editors author a schema and its rows, both on the explicit-save contract.** They are
  two of the seven `AssetEditorPanel` subclasses that host no dockspace (the four settings panels
  and the UI document editor above are the others). `TableSchemaEditorPanel`
  (`AssetTypes::TableSchema`) edits a `*.tableschema.json`'s column list — add / remove / rename /
  retype (a searchable picker over every registered non-`Reference` type) / reorder — plus the key
  column, whose combo is restricted to types a key index can order (`TableKeyKindForType`). It shows
  the resolved row layout and every validation failure live, and warns (without blocking) that a
  removed or retyped column invalidates tables already cooked against the schema — which is the
  *table's* cook error to surface. `DataTableEditorPanel` (`AssetTypes::DataTable`) is the grid: one
  column per schema column, **each cell drawn by the inspector widget for the column's reflected
  type** through `DrawFieldValue` — the label-less form of the shared field walk — so an enum column
  gets the named combo and an asset-handle column the asset picker with no per-column widget written
  here. A composite column (struct / variant / array) expands into several inspector rows and so
  cannot render inside one cell; it opens a popup holding a real `PropertyTable` and the ordinary
  `DrawFieldWidget` instead. Rows add / remove / duplicate / reorder, key cells are marked live when
  they duplicate an earlier row's, and the row body virtualizes through an `ImGuiListClipper`.
- **Both table panels validate through the importer's own rules, not a copy.** The panel documents
  live in `panels/TableDocument.{h,cpp}`, free of any UI dependency: `TableSchemaDocument::Resolve`
  calls **`Veng::LayOutTableSchema`** (the engine-tier function the cooker's `ParseTableSchema` also
  calls) and a cell binds through **`JsonReadFieldValue`** against a `ReflectedStorage`, so the
  diagnostics a panel shows are the cook's, character for character. The editor never links
  `libveng_cook`, so it re-parses the *authored* `*.tableschema.json` a table names rather than
  reading a cooked schema. Keeping the documents UI-free is also what makes the column/row
  operations and the whole save contract testable in the device-free `editor_unit` band.

## Project settings and preview capability

- **Project Settings is a host-level panel.** `ProjectSettingsPanel` (opened from the Window menu,
  like the asset browser) lists and edits the host-owned `ProjectSettings` — the array of
  `BuildConfiguration`s and the `ActiveConfiguration` selector — through the shared reflection
  inspector (`DrawFieldWidget` / `PropertyTable`): reflection draws the rows, the
  `CompressionRole` / `CompressionFormat` enum combos come from registered field widgets (the same
  way `LightType` does), and the configuration-array add/remove widget comes from the inspector's
  `FieldClass::Array` arm. Save round-trips `project.veng` (preserving the `packs` key) and
  rewrites each configuration's `*.buildcfg` at its referenced path (under `configs/`) through the
  editor's own nlohmann, using the shared enum⇄name tables the cooker parses by — so the editor
  *writes* exactly what the cooker *reads*.
- **Live preview is gated to host GPU capability.** Building any configuration is unrestricted
  (the encoder is CPU); *previewing through* one is bounded by what the host GPU can sample, so
  "ASTC on Windows" is structurally impossible rather than merely warned against.
  `editor/src/PreviewCapability.{h,cpp}` gates on the device **feature** (not a platform label) by
  reusing the engine's `Context::IsBlockCompressionSupported()` / `IsAstcSupported()` queries:
  `IsFormatPreviewable` / `IsConfigPreviewable` intersect a configuration's resolved role formats
  with the host's enabled features, and `HostSafeFormats` builds an uncompressed, always-samplable
  role table. The editor's default live-cook target is that host-safe profile — independent of
  which ship configuration is selected for editing — so it never hands the GPU an unsamplable
  blob. **"Preview as ship config" is opt-in** (the Project Settings panel's selector); a
  host-incompatible configuration is greyed out with the stated reason, and an all-incompatible
  project previews host-safe behind a banner. The same query is both the "active config not
  supported on this GPU" warning and the preview-eligibility gate.

## The node-graph UI and the material editor

The node-graph topology core, the material node catalog, and the Slang emit walk live in
**`veng::graph`** ([graph/CLAUDE.md](../graph/CLAUDE.md)), which the editor links PUBLIC. The
editor owns only the **UI**:

- **imnodes is used only by the editor, in `.cpp`** (its header never appears in a public
  header). imnodes.cpp is compiled **exactly once**, vendored into `libveng`'s ImGui aggregation
  TU, and `libveng_editor` imports those symbols across the PUBLIC `veng::veng` link (the include
  dir rides veng's PUBLIC interface). `libveng_editor` must **not** also link imnodes' own static
  target: that compiles a second imnodes.cpp into the editor, giving it a private `GImNodes` the
  engine's `ImNodes::CreateContext` never initializes — under two-level namespaces the editor's
  calls bind to that null context and crash. The only ImGui types in panel src are those crossing
  into imnodes (`ImVec2` for node positioning).
- **`MaterialEditorPanel` drives the imnodes canvas + a node-property inspector** reusing the
  per-`FieldClass` widgets. The graph (nodes, positions, property values, links) is embedded under
  an `"_editor"` key in the `.vmat.json`. The panel's cook-on-demand routes the graph source
  through the cooker's graph-shader resolver (`SetGraphShaderResolver`, installed at the editor's
  `veng::graph` link), so editing the graph regenerates the fragment Slang, recompiles it, and
  hot-reloads the result into `MaterialPreview` — the same walk the offline cook runs.
- **The material editor mints the `defaultInstance` id.** A parent material's companion
  default-instance id (the cook emits a zero-override `MaterialInstance` at it; every reference
  names it) lives in the `.vmat.json`. On save, `MaterialEditorPanel` backfills a missing id —
  minting a collision-free one against the project's packs through `EditorHost::MintAssetId` (an
  injected `AssetIdMinter` over the cooker's in-process `GenerateAssetId`, since `libveng_editor`
  never links the cooker) and writing it through the same preserve-unknown-keys `.vmat`
  round-trip. The id shows read-only in the panel beside the material id. A material never opened
  in the editor stays on the hand-mint floor (`vengc generate-id`, paste).
- **`MaterialPreview` renders one material on a sphere through an `Offscreen` `Viewport`** into an
  ImGui texture. It is **not** an `EditorPanel`, so its owning `MaterialEditorPanel` registers the
  viewport on its behalf; each frame the preview advances the turntable and pushes its
  `ViewState`, the engine renders the registered viewport, and `GetTexture()` samples the result.
  A save recooks off-thread and hot-reloads behind the stable `AssetHandle`, re-fetching the
  texture after a recompile/resize invalidates the output.
