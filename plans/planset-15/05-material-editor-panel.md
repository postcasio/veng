# Plan 05 — MaterialEditorPanel + integration (Layer 4)

**Goal:** the panel that joins everything — an imnodes canvas over the generic `NodeGraph`, a
node-property inspector, the live `MaterialPreview`, and the
edit→compile→`RequestCook`→hot-reload→preview loop — registered for `AssetType::Material` and
exercised by hello-triangle opening `brick.vmat.json`. This is the **integration join**
(needs plan 03 *and* plan 04). The panel owns no model truth (decision 2).

## imnodes build setup

imnodes is fetched but compiled nowhere. Add an aggregation TU
(`editor/src/Vendor/ImNodes.cpp`, source-including `imnodes.cpp`, mirroring
`engine/src/Vendor/ImGui.cpp`) and compile it into `libveng_editor` only. The runtime
(`libveng`, `libgame`) never links it. `imnodes.h` stays a src-only include — never in a
`VengEditor/` public header (the `NodeGraph` surface is imnodes-free).

## `MaterialEditorPanel`

`editor/src/panels/MaterialEditorPanel.{h,cpp}` — opened by double-clicking a material in the
asset browser (`AssetEditorFactory` for `AssetType::Material`, registered into
`EditorRegistry` in `EditorHost::OnInitialize`, first-write-wins like the texture editor). It
owns:

- The source `.vmat.json` path (via `AssetSourceIndex`) and the resolved shader interface
  (the material's `MaterialData` field list + shader ids).
- A `NodeGraph` + the material `NodeCatalog` (plan 03). On open: parse the `"_editor"` block
  if present, else `BuildGraphFromMaterial` from the flat fields (decision 8).
- A `MaterialPreview` (plan 04).
- The cook/hot-reload state mirroring `TextureEditorPanel`: a `MountHandle`, an
  `AssetHandle<Material>`, a 300ms debounce, a `m_Cooking` flag, an `optional<string>` error.

### The canvas

Each frame `OnImGui` projects the graph through imnodes: a node per `NodeGraph` node (title
from the catalog, pins from the type), links from `Links()`. User gestures translate into the
Layer-1 mutation vocabulary:

- imnodes "link created" → `NodeGraph::Connect` (a rejected `VoidResult` shows a transient
  toast; the visual link is not added).
- "link destroyed" → `Disconnect`; node drag → `MoveNode`; delete key → `RemoveNode`.
- A right-click "add node" menu lists `NodeCatalog::Types()`.

### The property inspector

A selected node's properties draw through the **same per-`FieldClass` widgets the entity
inspector uses** — factor the widget-drawing out of `InspectorPanel` into a shared helper
(`DrawFieldWidget(void* fieldPtr, const FieldDescriptor&, const EditorRegistry&)`) so both the
entity inspector and the node inspector call it (including the `AssetHandle` asset picker for
`TextureSample.Texture`). `RegisterFieldWidget` overrides apply to both.

### The live loop

Any mutation (connect/disconnect/property edit/node add/remove) marks the graph dirty and
arms the debounce. On expiry:
1. `CompileMaterialGraph` → the new `fields` JSON.
2. Patch it into a temp `.vmat.json` (existing `shaders` + a refreshed `"_editor"` block +
   the regenerated `fields`), preserving unknown keys.
3. `EditorHost::RequestCook` (off-thread) → on success a fresh `MountHandle`; `Load<Material>`
   behind the stable handle; on completion `MaterialPreview::SetMaterial`.
A cook error shows inline and logs to the console panel.

### Save

**Save** writes the patched `.vmat.json` to the real source (the `"_editor"` graph block +
the regenerated `fields`), preserving unknown keys — the planset-14 round-trip. **Revert**
re-parses the on-disk source and rebuilds the graph.

## hello-triangle migration

No new assets. `hello_triangle-editor` already loads `libhello_triangle`; verify
double-clicking `brick.vmat` opens the material editor, the synthesized graph shows the albedo
texture sample → output and the factors param, editing the factor or swapping the albedo
texture recooks and the preview sphere updates live, and Save embeds the graph + regenerates
`fields` without destroying the `shaders` block.

## Tests

- **Manual end-to-end:** the migration steps above (open, edit, live preview, save, reopen →
  graph round-trips).
- **Unit (device-free):** the panel's compile-on-edit path is covered by plan 03's tests; add
  a test that the shared `DrawFieldWidget` extraction leaves the entity inspector behavior
  unchanged (a smoke construction test, no GPU).
- `ctest` green; smoke PPM unchanged; `include_hygiene` green; validation gate clean.

## Acceptance

`hello_triangle-editor` opens `brick.vmat` in the node editor; the graph reflects the
material; editing nodes/properties recooks off-thread and the sphere preview updates live;
Save round-trips the `.vmat.json` (graph embedded, `fields` regenerated, `shaders` + unknown
keys preserved); reopening restores the graph; imnodes is linked only into `libveng_editor`;
`ctest` green; smoke PPM unchanged. Commit:
`Plan 05: MaterialEditorPanel — imnodes canvas, live compile+preview loop, hello-triangle migration`.
</content>
