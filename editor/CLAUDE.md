# libveng_editor — the editor framework

The editor is a separate executable, not part of the runtime. It is built on the
engine ([engine/CLAUDE.md](../engine/CLAUDE.md)) — `Veng::UI`, `SceneRenderer`,
`AssetManager`, and the reflection layer — and on the cook pipeline
([cooker/CLAUDE.md](../cooker/CLAUDE.md)) for its cook-on-demand loop. Project-wide
conventions live in the [root CLAUDE.md](../CLAUDE.md).

`libveng_editor` is the editor **framework** library; the `<name>-editor` exe
(produced by `veng_add_editor`) links `libveng`, `libveng_editor`, and `libveng_cook`,
and `dlopen`s the game module the same way the launcher does — but passing a non-null
`EditorRegistry*` in `VengModuleHost::Editor`.

- **`EditorHost`** is an `Application` subclass living in `libveng_editor`. It builds a
  top-level single-window `DockSpace` (`ImGuiConfigFlags_DockingEnable`; multi-viewport
  OS windows are not built — they conflict with the single-offscreen-composite model),
  owns the panel set (open/close state, Window menu, dock layout), and loads the game
  module with `Editor = &m_EditorRegistry`.
- **`EditorPanel`** is the panel base class: a `GetTitle()` / `OnImGui()` virtual interface,
  plus a `Draw(bool* open)` seam (default wraps `OnImGui` in one `UI::Window`) and an
  `OnRender(CommandBuffer&)` seam for a render-owning panel (a viewport records its scene
  render here, before the ImGui frame, so the output is sampleable when `OnImGui` draws it).
  The host drives each open panel and owns its visibility. Top-level host panels: asset
  browser, console/log, and the per-asset editors below.
- **`AssetEditorPanel` hosts a private, class-restricted dockspace.** An asset editor is a
  top-level panel whose window hosts a per-instance ImGui dockspace; its child panels are
  submitted as separate windows tagged with a per-instance `ImGuiWindowClass`, so only that
  editor's children dock into its area and cannot stray into the host dockspace (two open
  editors of the same asset stay isolated by a monotonic instance id). A subclass adds
  children with `AddChild` and arranges the initial split in `BuildDefaultLayout`; it
  overrides `Draw` to submit the document window + dockspace + the class-tagged children, and
  forwards `OnRender` to each child.
- **The prefab editor is the scene-editing surface.** `PrefabEditorPanel` (registered for
  `AssetType::Prefab`) loads + `SpawnInto`s the prefab into a document-owned live `Scene`
  (adding a default directional light when the prefab carries none) and hosts three children
  over one shared `PrefabEditContext` (`Scene*` + a multi-entity `Selection` + the `Active`
  entity + the `EntityPayload` drag tag): `SceneViewportPanel` (renders the scene from an
  orbit camera via a `SceneRenderer` into a `UI::Image`, with the `DebugView` dropdown),
  `PrefabExplorerPanel`, and `InspectorPanel`. The host opens the sample prefab as the initial
  document; double-clicking a prefab in the asset browser opens another.
- **`PrefabExplorerPanel` is a full scene-graph tree** over the intrusive `Hierarchy`
  ([engine/CLAUDE.md](../engine/CLAUDE.md)): roots are entities with a null parent, children
  walk `ForEachChild` in order. It drives the shared selection (click / Ctrl-click toggle),
  inline-renames (double-click), drag-reparents (`Scene::SetParent`) and reorders siblings
  (`Scene::MoveBefore`) — both with a cycle pre-check so the engine's fatal cycle assert is
  never reached — and creates / adds-child / duplicates / deletes entities, plus a name
  filter and row/empty-space context menus. Every structural edit is **queued during the
  draw and applied after** the snapshot walk returns, so nothing mutates the scene
  mid-iteration (the `Scene` contract).
- **Reflection-driven inspector.** `InspectorPanel` edits `PrefabEditContext::Active`: an
  editable name header, a searchable **Add Component** picker (every registered
  `FieldClass::Struct` type not already present, minus the hierarchy-owned `Hierarchy`), and
  per-component remove / reset-to-default — the remove queued and applied after the
  `Scene::ForEachComponent` walk. Each component's fields render in a two-column
  `UI::PropertyTable` via the shared `DrawFieldWidget` helper (`editor/src/FieldWidget.{h,cpp}`,
  taking a `FieldWidgetContext { AssetManager&, const AssetSourceIndex&, const EditorRegistry& }`),
  which draws a built-in widget per `FieldClass`
  (Scalar/Vector/Quaternion/String/AssetHandle/Enum/Reference/Struct/Matrix/Variant), honors
  `FieldDescriptor::ReadOnly`/`Hidden`/`Tooltip`, recurses nested structs as flattened indented
  rows, makes enums editable (with a registered `LightType` combo), and turns `Reference`
  fields into Entity drop targets. The **`Variant` widget** is a combo over the alternatives'
  display names (plus "(none)") that `SetActive`s the chosen alternative on change and recurses
  the active member's fields as indented rows — so a `PrimitiveComponent`'s shape variant gives
  primitive-kind selection and per-shape parameter editing for free, and editing re-resolves the
  mesh through the editor's per-frame `ResolvePrimitiveMeshes`. A `RegisterFieldWidget` entry
  overrides the built-in for a given `TypeId`; the entity inspector and the node-property
  inspector both call `DrawFieldWidget`, so the two share identical widget behavior. The
  `AssetHandle` widget is an asset **picker** (a combo over the `AssetSourceIndex` entries of the
  field's `AssetType`), not a read-only label.
- **`EditorRegistry`** is defined in `libveng_editor` and **forward-declared** in
  `engine/include/Veng/Module/Module.h` (so `libveng` stays clean). It holds the
  `AssetType`→editor-factory map (double-click an asset opens its editor),
  `RegisterPanel` for game-contributed panels, and `RegisterFieldWidget(TypeId,
  FieldWidgetFn)` for custom inspector widgets. It is non-null in `VengModuleHost` only
  in the editor host.
- **Reflection-driven inspector.** Selecting an entity walks its components through the
  host-owned `TypeRegistry` / `FieldDescriptor` layer (`Scene::ForEachComponent`), drawing
  a built-in widget per `FieldClass`
  (Scalar/Vector/Quaternion/String/AssetHandle/Enum/Struct/Matrix/Reference); a
  `RegisterFieldWidget` entry overrides the built-in for a given `TypeId`. The per-field draw
  is the shared `DrawFieldWidget` helper (`editor/src/FieldWidget.{h,cpp}`, taking a
  `FieldWidgetContext { AssetManager&, const AssetSourceIndex&, const EditorRegistry& }`) —
  the entity inspector and the node-property inspector both call it, so the two share
  identical widget behavior. The `AssetHandle` widget is an asset **picker** (a combo over
  the `AssetSourceIndex` entries of the field's `AssetType`), not a read-only label.
- **Cook-on-demand keeps the importer boundary.** `libveng_cook` is linked **only into
  the editor exe** — never `libveng_editor`, never `libgame` — so the editor framework
  library stays importer-free. The exe injects a `CookBackend` implementation;
  `EditorHost::RequestCook(CookRequest, callback)` cooks a single source off the render
  thread via `TaskSystem` (`CookSession` → `Task<vector<u8>>`), then mounts the resulting
  in-memory archive via `AssetManager::MountMemory` and hot-reloads behind the stable
  `AssetHandle`.
- **The texture editor is the template.** `TextureEditorPanel` previews via a render
  target (`CreateTexture` → `ImGui::Image`), edits `.tex.json` settings (sRGB + sampler
  filter/wrap), recooks live (300ms-debounced), and round-trips the JSON on save —
  patching known keys, preserving unknown ones.
- **`VengEditor/NodeGraph/` is a named, reusable node-graph surface.** A generic,
  imnodes-free, device-free **topology core** (`NodeGraph` — generational `NodeId`, `PinType`,
  the mutation vocabulary, direction/arity/acyclicity validation, a construction-time
  `CanConnectFn`/`PinShapeFn`/`PropertySizeFn` hook set) + a data-driven `NodeType`/
  `NodeCatalog` + graph (de)serialization to/from a JSON document string (the public surface
  stays free of the JSON library type). It is reusable by any editor. The material editor
  (and future editors — the scene editor) consume it from editor src. imnodes is used **only
  by the editor** (linked PRIVATE into `libveng_editor`, src-only — its header never appears
  in a `VengEditor/` public header; its symbols are vendored in `libveng`'s ImGui aggregation
  TU and imported across the PUBLIC `veng::veng` link).
- **Node types are data, not subclasses.** A `NodeType` is pins (typed in/out) + a reflected
  property struct; a node instance stores property bytes the reflection serializer and
  inspector widgets walk, exactly like an ECS component. `NodeTypeId` is editor-local
  (defined in `NodeGraph.h`), distinct from the runtime `TypeId` space; pin data types reuse
  builtin leaf `TypeId`s.
- **The material editor authors a graph compiled to a `.vmat` field list** (v1 binds params
  to an author-provided shader — no node→Slang codegen). The graph (nodes, positions,
  property values, links) is embedded under an `"_editor"` key in the `.vmat.json`, and
  `fields` is regenerated on compile (reusing the texture editor's preserve-unknown-keys JSON
  round-trip). `MaterialEditorPanel` drives an imnodes canvas over the graph and a
  node-property inspector reusing the per-`FieldClass` widgets. Textures are node properties
  (`FieldClass::AssetHandle`), not wired pins, so the topology core stays asset-agnostic. The
  node catalog is **domain-aware** (`RegisterMaterialNodeTypes` takes the domain): the
  `MaterialOutput` node's pins follow the domain's output contract (`Color` for PostProcess)
  rather than only mirroring the loaded shader's `GetFields()`, and compile writes the
  `"domain"` key. Node→Slang codegen — every node an expression emitter generating the
  fragment source — remains the named follow-on.
- **`MaterialPreview` renders one material on a sphere via `SceneRenderer`** into an ImGui
  texture; the edit loop recooks off-thread and hot-reloads behind the stable `AssetHandle`,
  re-fetching the texture after the recompile/resize invalidates `GetOutput()`.
