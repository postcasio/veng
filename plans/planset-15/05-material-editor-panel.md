# Plan 05 — MaterialEditorPanel + integration (Layer 4)

**Goal:** the panel that joins everything — an imnodes canvas over the generic `NodeGraph`, a
node-property inspector, the live `MaterialPreview`, and the
edit→compile→cook→hot-reload→preview loop — registered for `AssetType::Material` and
exercised by hello-triangle opening `brick.vmat.json`. This is the **integration join** (needs
plan 03, plan 04, and the variable params from plan 00). The panel owns no model truth
(decision 2).

## imnodes build setup

imnodes is fetched with its own CMake, which **already defines an `imnodes` library target**
(`EXCLUDE_FROM_ALL`, taking imgui includes from the existing `imgui_headers` INTERFACE target);
nothing links it yet, so it is built nowhere. **Link that `imnodes` target into
`libveng_editor`** — `PRIVATE`, so it is not part of the editor framework's public surface; its
unresolved ImGui symbols bind against `veng::veng` (which hosts the ImGui symbols) at the final
link. Do **not** add a source-include aggregation TU — that would double-compile `imnodes.cpp`.
The runtime (`libveng`, `libgame`) never links imnodes. `imnodes.h` stays a src-only include —
never in a `VengEditor/` public header (the `NodeGraph` surface is imnodes-free).

## `MaterialEditorPanel`

`editor/src/panels/MaterialEditorPanel.{h,cpp}` — opened by double-clicking a material in the
asset browser (`AssetEditorFactory` for `AssetType::Material`, registered into
`EditorRegistry` in `EditorHost::OnInitialize`, first-write-wins like the texture editor). It
is constructed by the factory with a **`CookDriver` callback** (the existing alias
`TextureEditorPanel` takes, which `EditorHost` wires to its `RequestCook`) — the panel never
names `EditorHost::RequestCook` directly, since a factory-built panel holds no `EditorHost&`.
It owns:

- The source `.vmat.json` path (via `AssetSourceIndex`) and the `MaterialShaderInterface` (plan
  03) — `Material::GetFields()` of the loaded material + its shader ids.
- A `NodeGraph` + the material `NodeCatalog` (plan 03). On open: parse the `"_editor"` block if
  present (honouring its `"version"` — a newer version opens **read-only**, no regeneration),
  else `BuildGraphFromMaterial` from the loaded field table (decision 8). Opening never rewrites
  `fields`.
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

### The property inspector + the asset picker

A selected node's properties draw through the **same per-`FieldClass` widgets the entity
inspector uses** — factor the widget-drawing out of `InspectorPanel` into a shared helper so
both the entity inspector and the node inspector call it. The helper needs more than the
registry: it dispatches `RegisterFieldWidget` overrides and recurses for nested structs (so it
needs the `EditorRegistry`), and the new `AssetHandle` picker enumerates candidate ids of the
field's `AssetType` from the `AssetSourceIndex` (so the context carries it). It takes a small
context:

```cpp
struct FieldWidgetContext
{
    Veng::AssetManager& Assets;       // for handle widgets that resolve the referenced asset
    const AssetSourceIndex& Sources;  // the picker's type-filtered candidate enumeration
    const EditorRegistry& Editors;    // RegisterFieldWidget overrides + struct recursion
};
void DrawFieldWidget(void* fieldPtr, const Veng::FieldDescriptor&, const FieldWidgetContext&);
```

It recurses through itself for nested structs and dispatches `RegisterFieldWidget` overrides —
both panels share the same behavior. The extraction is covered by a test that the entity
inspector's field walk is unchanged.

`InspectorPanel` is currently constructed with `(AssetManager&, EditorRegistry&)` only; to build
the context (and so gain the picker for the entity inspector) it also takes the `AssetSourceIndex`
the host already owns and parses.

**The `AssetHandle` picker is new work** (decision 6): the inspector's current
`FieldClass::AssetHandle` case is a **read-only label** that reads only the leading `AssetId` off
the field pointer (it consults neither `AssetManager` nor any candidate source today — the picker
is genuinely new, not a refactor of existing behaviour). This planset replaces it with a real
picker — a combo/popup listing candidate `AssetId`s of the field's `AssetType`, writing the
chosen id back through the field pointer. Candidates come from `AssetSourceIndex`, which gains a
**type-filtered enumeration** accessor (it currently only exposes `Find`; add a `ForEach`/
`Entries`-by-`AssetType` walk over its `m_Entries`). The picker lands in the shared helper, so
the entity inspector gains it too. `TextureSample.Texture` uses it to set the albedo texture.

### The live loop

Any mutation (connect/disconnect/property edit/node add/remove) marks the graph dirty and
arms the debounce. An edit during an in-flight cook re-arms it, firing one more cook on
completion (the texture editor's coalescing). On expiry:
1. `CompileMaterialGraph` → the typed field list (plan 03).
2. Serialize it into a temp `.vmat.json` (existing `shaders` + a refreshed `"_editor"` block +
   the regenerated `fields`), preserving unknown keys. The temp file is written **into the real
   source's directory** so the importer's relative `.shader.json` resolution
   (`source-dir`-relative) still resolves. It uses a single fixed dotfile name per editor
   instance (e.g. `.<stem>.editor-tmp.vmat.json`, overwritten each cook, not a fresh file per
   keystroke) and is removed when the panel closes — so live editing leaves no stray sources in
   the asset tree and never collides with the real `.vmat.json` (which Save, step below, writes).
3. Drive the `CookDriver` callback (off-thread) → on success a fresh `MountHandle`;
   `Load<Material>` behind the stable handle; on completion `MaterialPreview::SetMaterial`.
A cook error shows inline and logs to the console panel.

### Save

**Save** writes the patched `.vmat.json` to the real source (the versioned `"_editor"` graph
block + the regenerated `fields`), preserving unknown keys — the planset-14 round-trip. A
material opened from a *newer* `"_editor"` version is read-only and Save is disabled (decision
8), so a stale editor never regenerates a degraded `fields`. **Revert** re-parses the on-disk
source and rebuilds the graph.

## hello-triangle migration

No new assets. `hello_triangle-editor` already loads `libhello_triangle`; verify
double-clicking `brick.vmat` opens the material editor, the synthesized graph shows the albedo
texture sample → output and the factors param, editing the factor or swapping the albedo
texture recooks and the preview sphere updates live, and Save embeds the graph + regenerates
`fields` without destroying the `shaders` block. The manual demo is **not** committed as a
mutated `brick.vmat.json` — the checked-in source (shared by `examples/`, `tests/cooker/
fixtures/`, `tests/gpu/assets/`) stays as hand-authored; the import→compile round-trip is
field-stable (plan 03's identity check), so reopening after a no-op edit leaves it byte-equal.

## Tests

- **Manual end-to-end:** the migration steps above (open, edit, live preview, save, reopen →
  graph round-trips).
- **Unit (`veng_editor_unit`, device-free):** the panel's compile-on-edit path is covered by
  plan 03's tests; add a test driving the shared `DrawFieldWidget` over a struct with an
  `AssetHandle` field and asserting the picker writes the chosen id back through the field
  pointer (the extraction leaves the entity inspector's field walk unchanged), plus an
  `AssetSourceIndex` type-filtered-enumeration test.
- `ctest` green; smoke PPM unchanged; `include_hygiene` green; validation gate clean.

## Acceptance

`hello_triangle-editor` opens `brick.vmat` in the node editor; the graph reflects the
material; editing nodes/properties recooks off-thread and the sphere preview updates live;
Save round-trips the `.vmat.json` (graph embedded, `fields` regenerated, `shaders` + unknown
keys preserved); reopening restores the graph; imnodes is linked only into `libveng_editor`;
`ctest` green; smoke PPM unchanged. Commit:
`Plan 05: MaterialEditorPanel — imnodes canvas, live compile+preview loop, hello-triangle migration`.
</content>
