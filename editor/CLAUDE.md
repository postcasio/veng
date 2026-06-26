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
  module with `Editor = &m_EditorRegistry`. **It runs the engine render tail**, not a
  hand-rolled present path: a render-owning panel holds its own `Viewport` and registers
  it into the base `Application` drive-list, so the engine renders every panel viewport,
  then `ImGuiLayer::Render` + the managed gather/composite bracket `OnRender` in the base.
  The editor registers **no** `Presented` viewport, so the composite runs with **zero
  placements** (a cleared assembly target, ImGui only) — the editor's content is the
  ImGui dockspace, and each panel's scene is an ImGui texture over an `Offscreen` viewport.
- **`EditorPanel`** is the panel base class: a `GetTitle()` / `OnUI()` virtual interface
  plus a `Draw(bool* open)` seam (default wraps `OnUI` in one `UI::Window`). It carries
  **no render seam** — a render-owning panel holds a registered
  `Veng::Renderer::Viewport` (`Offscreen`) and the engine drive-list renders it each
  frame, so the panel records no scene render of its own; it pushes the viewport's
  `ViewState` and its region (from the ImGui content rect) in `OnUI`, and samples the
  ready output as a `UI::Image`. The host drives each open panel and owns its visibility.
  Top-level host panels: asset browser, console/log, and the per-asset editors below.
- **`AssetEditorPanel` hosts a private, class-restricted dockspace.** An asset editor is a
  top-level panel whose window hosts a per-instance ImGui dockspace; its child panels are
  submitted as separate windows tagged with a per-instance `ImGuiWindowClass`, so only that
  editor's children dock into its area and cannot stray into the host dockspace (two open
  editors of the same asset stay isolated by a monotonic instance id). A subclass adds
  children with `AddChild` and arranges the initial split in `BuildDefaultLayout`; it
  overrides `Draw` to submit the document window + dockspace + the class-tagged children.
  A child that renders a scene owns its own registered `Offscreen` viewport, so there is
  no render forwarding — the engine renders every panel viewport before the ImGui frame.
- **The prefab editor is the scene-editing surface.** `PrefabEditorPanel` (registered for
  `AssetType::Prefab`) loads + `SpawnInto`s the prefab into a document-owned live `Scene`
  (adding a default directional light when the prefab carries none) and hosts three children
  over one shared `PrefabEditContext` (`Scene*` + `AssetManager*` + a multi-entity `Selection` +
  the `Active` entity + the `EntityPayload` drag tag + a `ResolveEntity` helper): `SceneViewportPanel`
  (owns a registered `Offscreen` `Viewport` the engine renders, samples its output into a
  `UI::Image`, feeds the viewport's region from the panel content rect and pushes an
  orbit-camera `ViewState` each frame, with the `DebugView` dropdown; `Viewport::ScreenToWorldRay`
  is the entity-picking seam now available to it), `PrefabExplorerPanel`, and `InspectorPanel`.
  The host opens the sample prefab as the initial document; double-clicking a prefab in the
  asset browser opens another.
- **The level editor is the game-wiring surface.** `LevelEditorPanel` (registered for
  `AssetType::Level`) **derives from** `PrefabEditorPanel`, so the viewport / explorer /
  inspector edit the level's **world prefab** with no scene-editing reimplemented, and adds two
  level-scoped children over the same dockspace: a **systems panel** listing the
  `SystemRegistry` catalog ([engine/CLAUDE.md](../engine/CLAUDE.md)) with a per-system enable
  toggle, phase labels, and drag-reorder over the active set — writing the level's ordered
  `SystemId` list — and a **settings panel** drawing the `GameModeConfig` and
  `LevelRenderSettings` through the shared reflection inspector (`DrawFieldWidget`). System
  *params* stay components, edited through the world surface like any other; the level editor
  adds **no new inspector machinery** — the catalog drives the systems panel and reflection
  draws the config. Editing recooks the `*.level.json` off the render thread and hot-reloads
  behind the stable handle (the round-trip preserves unknown keys, like the texture editor).
  Play runs **exactly the level's ordered system set** through the base's play machinery
  (`GetPlaySystems`), distinct from a bare prefab document's "all registered" set.
- **`PrefabExplorerPanel` is a full scene-graph tree** over the intrusive `Hierarchy`
  ([engine/CLAUDE.md](../engine/CLAUDE.md)): roots are entities with a null parent, children
  walk `ForEachChild` in order. It drives the shared selection (click / Ctrl-click toggle),
  inline-renames (double-click), drag-reparents (`Scene::SetParent`) and reorders siblings
  (`Scene::MoveBefore`) — both with a cycle pre-check so the engine's fatal cycle assert is
  never reached — and creates / adds-child / duplicates / deletes entities, plus a name
  filter and row/empty-space context menus. Every structural edit is **queued during the
  draw and applied after** the snapshot walk returns, so nothing mutates the scene
  mid-iteration (the `Scene` contract). Duplicating an entity round-trips its components into
  the copy but builds no derived mesh, so `DuplicateSubtree` calls `ResolveEntity` on each
  copy after its children recurse — the byte copy carries the recipe `Source` forward but
  never the built handle.
- **A mesh source re-resolves like any asset field.** A `MeshRenderer` carrying an inline
  recipe `Source` ([engine/CLAUDE.md](../engine/CLAUDE.md)) builds its mesh during
  `Prefab::SpawnInto`'s populate pass; there is no per-frame scan. An inspector edit to the
  source repoints the mesh exactly as repointing a cooked `AssetHandle` field would, so two
  of the editor's resolve triggers collapse into "the source re-resolves on the changed-bool":
  Add Component (`InspectorPanel`) and an inspector field edit (`InspectorPanel`, gated on the
  `DrawFieldWidget` changed-bool). The **duplicate** path is different — a `DuplicateSubtree`
  byte copy has no inspector edit and no changed-bool to hook, so it rebuilds the derived mesh
  from the copied source explicitly. All three funnel through `PrefabEditContext::ResolveEntity`,
  which rebuilds the entity's `Mesh` from a non-empty `Source` via `BuildPrimitiveMesh`; a
  future structural op (paste, undo) adds its own trigger.
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
  the active member's fields as indented rows — so a `MeshRenderer`'s `Source` shape variant
  gives primitive-kind selection and per-shape parameter editing for free. `DrawFieldWidget`
  returns a `bool changed` (accumulated through its nested-struct/variant recursion);
  `DrawComponent` ORs it across the component's fields and, when true, calls
  `PrefabEditContext::ResolveEntity` so an edit to a recipe source (its shape/parameters)
  rebuilds the derived mesh. A `RegisterFieldWidget`
  entry overrides the built-in for a given `TypeId`; the entity inspector and the node-property
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
  patching known keys, preserving unknown ones. It carries a **compression-role combo**
  over the same round-trip (writing/clearing the `"role"` key) and shows the **resolved
  format read-only** for the active configuration ("→ ASTC4x4Srgb for 'macos'"), so the
  artist picks intent and reads the platform's codec without choosing one.
- **The editor opens the project, not a manifest.** `EditorHostInfo::ProjectPath` (the
  `project.veng`, baked absolute by `veng_add_editor`) is the editor's entrypoint:
  `EditorHost` reads it through `LoadProjectSettings` (the host-owned `ProjectSettings` — its
  `Configurations`, `ActiveConfiguration`, `Packs`, `StartupLevel`), mounts each cooked pack the
  project names (beside the exe, under the source manifest's stem), and builds its
  `AssetSourceIndex` from the **union** of the project's pack manifests
  (`AssetSourceIndex::ParsePacks`). The runtime `.vengproj` is a game-launch artifact the editor
  does not consume.
- **Project Settings is a host-level panel.** `ProjectSettingsPanel` (opened from the
  Window menu, like the asset browser) lists and edits the host-owned `ProjectSettings` —
  the array of `BuildConfiguration`s and the `ActiveConfiguration` selector — through the
  shared reflection inspector (`DrawFieldWidget` / `PropertyTable`): reflection draws the
  rows, the `CompressionRole` / `CompressionFormat` enum combos come from registered field
  widgets (the same way `LightType` does), and the configuration-array add/remove widget
  comes from the inspector's `FieldClass::Array` arm. Save round-trips `project.veng`
  (preserving the `packs` key) and rewrites each configuration's `*.buildcfg` at its referenced
  path (under `configs/`) through the editor's own nlohmann, using the shared enum⇄name tables
  the cooker parses by — so the editor *writes* exactly what the cooker *reads*.
- **Live preview is gated to host GPU capability.** Building any configuration is
  unrestricted (the encoder is CPU); *previewing through* one is bounded by what the host
  GPU can sample, so "ASTC on Windows" is structurally impossible rather than merely warned
  against. `editor/src/PreviewCapability.{h,cpp}` gates on the device **feature** (not a
  platform label) by reusing the engine's `Context::IsBlockCompressionSupported()` /
  `IsAstcSupported()` queries: `IsFormatPreviewable` / `IsConfigPreviewable` intersect a
  configuration's resolved role formats with the host's enabled features, and
  `HostSafeFormats` builds an uncompressed, always-samplable role table. The editor's
  default live-cook target is that host-safe profile — independent of which ship
  configuration is selected for editing — so it never hands the GPU an unsamplable blob.
  **"Preview as ship config" is opt-in** (the Project Settings panel's selector); a
  host-incompatible configuration is greyed out with the stated reason, and an
  all-incompatible project previews host-safe behind a banner. The same query is both the
  "active config not supported on this GPU" warning and the preview-eligibility gate.
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
- **`MaterialPreview` renders one material on a sphere through an `Offscreen` `Viewport`**
  into an ImGui texture. It is **not** an `EditorPanel`, so its owning `MaterialEditorPanel`
  registers the viewport on its behalf; each frame the preview advances the turntable and
  pushes its `ViewState`, the engine renders the registered viewport, and `GetTexture()`
  samples the result. The edit loop recooks off-thread and hot-reloads behind the stable
  `AssetHandle`, re-fetching the texture after a recompile/resize invalidates the output.
