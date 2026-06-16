# Plan 06 — docs + roadmap re-cut

**Goal:** update the roadmap and `CLAUDE.md` to reflect planset-15's deliverables. No code.

## `plans/README.md`

Add the planset-15 entry after planset-14:

> **planset-15** — node-based material editor (✅ done, 7 plans). Delivers
> [future area 6](future/README.md#6-editor-application) sub-area C, the **loaded `.vmat`
> path**: a node-graph material editor on a new **named `libveng_editor` surface**
> (`VengEditor/NodeGraph/`), over a precursor that splits material parameters into a fixed
> **engine-supplied** block and a variable-size **authored** block (so a graph can author
> uniforms beyond the one `vec4 Factors`). Four node-graph layers with hard dependency ceilings — a pure, device-free,
> unit-tested **topology core** (`NodeGraph`, generational `NodeId`, `PinType`, the mutation
> vocabulary, direction/arity/acyclicity validation, a domain-supplied `CanConnect` hook); a
> generic **data-driven catalog** (`NodeType` = pins + a reflected property struct, node
> instances stored as bytes walked by the existing reflection serializer/inspector widgets,
> graph (de)serialization); the **material specialization** (the `TextureSample`/`Param`/`MaterialOutput`
> catalog, coercion-aware connection, `CompileMaterialGraph` → a `.vmat` field list, and a
> flat-`.vmat`→graph import); and the **`MaterialEditorPanel`** (imnodes canvas, node-property
> inspector reusing the per-`FieldClass` widgets, live compile→cook→hot-reload→preview against
> a reusable `MaterialPreview` sphere). Textures are node **properties** of
> `FieldClass::AssetHandle`, not wired pins (the topology core stays asset-agnostic); the
> graph is embedded under an `"_editor"` key in the `.vmat.json` and compile regenerates the
> `fields` array, reusing planset-14's preserve-unknown-keys round-trip. v1 binds parameters
> to an author-provided shader (no node→Slang codegen). hello-triangle's `brick.vmat` opens,
> edits, previews live, and saves through the editor. imnodes (its own library target) is linked
> only into `libveng_editor`; the runtime never links it. Held back: shader-graph codegen, wired asset
> pins, undo/redo, and the scene editor (sub-area D).

## `plans/future/editor.md`

Mark sub-area C (the node-based material editor, the "headline" section) **delivered
(planset-15)**: the node-graph surface, the material catalog/compile, the live preview, and the
variable engine/authored material-param split landed. Leave the section's design notes; add the
delivered marker and point forward to sub-area D (scene editor) as the remaining editor work,
mirroring how planset-14 marked sub-area B delivered here.

## `plans/future/README.md`

- **Area 6:** mark sub-area C **delivered (planset-15)**. The only remaining editor sub-area
  is **D — scene editor** (viewport reusing `SceneRenderer`, hierarchy, gizmos, `.prefab.json`
  save round-trip); all its gates are met. Note the generic `VengEditor/NodeGraph/` surface as
  reusable by D and game editor modules.
- **Ordering section:** update `remaining:` to show sub-C delivered and sub-D as the next
  editor planset.
- **Status paragraph:** name planset-15 as delivering sub-area C and the loaded-`.vmat` path;
  identify the scene editor (sub-area D) as the next editor planset.

## `CLAUDE.md`

Extend the **Editor** section:

- The node-graph framework is a named surface: `VengEditor/NodeGraph/` — a generic,
  imnodes-free, device-free topology core + data-driven `NodeType`/`NodeCatalog` + graph
  serialization, reusable by any editor. The material editor (and future editors) are
  consumers in editor src; imnodes (its own library target) is linked only into `libveng_editor`.
- Node types are data (pins + a reflected property struct), not subclasses; node instances
  store property bytes walked by the reflection serializer and inspector widgets. `NodeTypeId`
  is editor-local, distinct from the runtime `TypeId` space; pin data types reuse builtin leaf
  `TypeId`s.
- The material editor authors a graph whose compiled output is a `.vmat` field list (v1 binds
  params to an author-provided shader); the graph is embedded under `"_editor"` in the
  `.vmat.json` and `fields` is regenerated on compile. Textures are node properties
  (`FieldClass::AssetHandle`), not wired pins.
- `MaterialPreview` renders one material on a sphere via `SceneRenderer` into an ImGui
  texture; the edit loop recooks off-thread and hot-reloads behind the stable handle.

Extend the **Shaders & materials** section: a material's GPU parameters are two parallel
SSBO entries indexed by the material slot — the fixed **engine-supplied `MaterialData`** block
(bindless handle slots the loader patches; `static_assert`-pinned; the `static_assert` byte
size is updated) and a variable-size **authored `MaterialParams`** block (the shader's declared
scalar/vector uniforms, reflected per-shader, byte-addressed at `MaterialParamStride`). The
cooker reflects both; `Material::GetFields()` exposes the reflected table (the editor's
parameter-schema source, no Slang in `libveng_editor`).

Document the shared `DrawFieldWidget` helper in the inspector note (entity inspector and node
inspector share it) and that the inspector's `AssetHandle` widget is now an asset **picker**
(combo over `AssetSourceIndex` entries of the field's `AssetType`), not a read-only label.

## Acceptance

`plans/README.md` has the planset-15 entry; `plans/future/editor.md` marks sub-area C delivered;
`plans/future/README.md` area 6 marks sub-area C delivered and identifies sub-area D as next;
`CLAUDE.md` documents the `VengEditor/NodeGraph/` surface, the material editor, `MaterialPreview`,
the engine/authored material-param split + `Material::GetFields()`, and the inspector asset
picker. No code changes. Commit:
`planset-15: docs + roadmap re-cut — node-based material editor delivered (sub-area C), scene editor next`.
</content>
